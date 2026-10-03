// Copyright 2025 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package xattr

import (
	"bytes"
	"errors"
	"fmt"
	"io/fs"
	"log"
	"os/exec"
	"path/filepath"
	"runtime"
	"sync"
	"syscall"
	"time"

	"golang.org/x/sys/unix"

	"github.com/google/abfs-casfs/api"
	"github.com/google/abfs-casfs/api/tapping"
)

const (
	ChannelMultiplier = 4
	MinimumChannel    = 64
)

func RunTap(cmd *exec.Cmd, hasher api.Hasher, tapPath string, pidInfo func(int), eventHandler func(t *api.TapFSEntry)) (*api.TapInfo, error) {
	execStart := time.Now()

	if err := syscall.Setpgid(0, 0); err != nil {
		return nil, err
	}

	fd, err := unix.Open(tapPath, unix.O_RDWR|unix.O_TMPFILE|unix.O_EXCL|unix.O_CLOEXEC, 0600)
	if err != nil {
		return nil, fmt.Errorf("failed to open tmp file for dir %s (%s)", tapPath, err)
	}
	defer unix.Close(fd)

	stdout := new(bytes.Buffer)
	stderr := new(bytes.Buffer)
	cmd.Stdout = stdout
	cmd.Stderr = stderr

	// Spawn reading of tap data
	var wg sync.WaitGroup
	var decodeErr error
	wg.Add(2)

	channelCapacity := runtime.NumCPU() * ChannelMultiplier
	if channelCapacity < MinimumChannel {
		channelCapacity = MinimumChannel
	}
	taps := make(chan *api.TapFSEntry, channelCapacity)

	tapReader := tapping.TapReaderCreate(fd)

	go func() {
		defer wg.Done()
		defer close(taps)

		decodeErr = tapping.TapDecode(tapReader, hasher, taps)
	}()
	go func() {
		defer wg.Done()
		defer func() {
			// Drain taps in case of panic
			for range taps {
			}
		}()

		for t := range taps {
			eventHandler(t)
		}

	}()

	var exitErr *exec.ExitError
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	if pidInfo != nil {
		pidInfo(cmd.Process.Pid)
	}
	if err := cmd.Wait(); err != nil && !errors.As(err, &exitErr) {
		return nil, err
	}

	info := &api.TapInfo{
		Pid:       cmd.Process.Pid,
		ExitCode:  cmd.ProcessState.ExitCode(),
		Rusage:    cmd.ProcessState.SysUsage().(*syscall.Rusage),
		Timestamp: execStart,
		Stdout:    stdout.String(),
		Stderr:    stderr.String(),
	}
	// Tell reader that once the events drain we are done
	if err := tapReader.CmdDone(); err != nil {
		log.Printf("Fatal kernel communication error: %v", err)
		return nil, err
	}
	wg.Wait()
	if decodeErr != nil {
		return nil, decodeErr
	}
	info.RunDuration = time.Since(execStart).Seconds()
	return info, nil
}
func RunTapCmd(cmd *exec.Cmd, hasher api.Hasher, tapPath string, pidInfo func(int)) (*api.TapCmdResult, error) {

	tapResult := &api.TapCmdResult{}

	eventHandler := func(t *api.TapFSEntry) {
		tapResult.TapEntries = append(tapResult.TapEntries, t)
	}

	info, err := RunTap(cmd, hasher, tapPath, pidInfo, eventHandler)
	if err != nil {
		return nil, err
	}
	tapResult.TapInfo = *info
	return tapResult, nil
}

func RunTapCmdResolved(cmd *exec.Cmd, casfs api.FS, hasher api.Hasher, tapPath string, pidInfo func(int)) (*api.TapCmdResolvedResult, error) {
	out := api.NewTapCmdResolvedResult()
	filterMap := make(map[string]struct{})
	tracker := api.NewTapTrack()
	updateRead := func(t *api.TapFSEntry) {
		filter := false
		// Check if path already exists as a write
		_, filter = filterMap[t.Path]
		if filter {
			return
		}
		// Add to read path
		out.Reads[t.Path] = api.HashStat{Hash: t.Hash, Mode: t.Mode, Size: t.Size}
	}
	updateWrite := func(t *api.TapFSEntry) {
		filterMap[t.Path] = struct{}{}
		tracker.Add(t.Path, t.Mode)
	}
	removeWrite := func(path string) {
		tracker.Remove(path)
	}
	updateDelete := func(path string) {
		tracker.Remove(path)
		filterMap[path] = struct{}{}
		out.Deletes[path] = struct{}{}
	}
	eventHandler := func(t *api.TapFSEntry) {
		switch t.Type {
		// Read events
		case api.TapFSOpenRead, api.TapFSStat, api.TapFSSymlinkRead, api.TapFSReadDir:
			updateRead(t)
		case api.TapFSMkdir, api.TapFSOpenWrite, api.TapFSSymlink:
			// Add write data
			updateWrite(t)
		case api.TapFSUnlink:
			// remove any write record we have
			removeWrite(t.Path)
			// and add to delete
			updateDelete(t.Path)
		case api.TapFSRename:
			// Add delete and write record
			tracker.Rename(t.AuxPath, t.Path, t.Mode)
			filterMap[t.AuxPath] = struct{}{}
			filterMap[t.Path] = struct{}{}
			out.Deletes[t.AuxPath] = struct{}{}

		case api.TapFSAttrChange:
			// Add a read record and a write record
			updateRead(t)
			updateWrite(t)
		}
	}

	info, err := RunTap(cmd, hasher, tapPath, pidInfo, eventHandler)
	if err != nil {
		return nil, err
	}
	hashStart := time.Now()

	// Hash any write data by walking the tree
	walkFunc := func(path string, mode fs.FileMode) error {
		if mode.IsDir() {
			// Don't hash directories just add to our write list
			out.Writes[path] = api.HashStat{Mode: mode,
				Hash: hasher.EmptyDirHash(),
			}
		} else {
		retry:
			hs, err := casfs.HashStat(filepath.Join(casfs.MountPath(), path))
			if err != nil {
				// Add test case to cover this
				if err == syscall.EINTR {
					goto retry
				}
				return err
			}
			out.Writes[path] = *hs
		}
		return nil

	}
	if err := tracker.Walk(walkFunc); err != nil {
		return nil, err
	}
	info.HashDuration = time.Since(hashStart).Seconds()
	out.TapInfo = *info
	return out, nil
}
