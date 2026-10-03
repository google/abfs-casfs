// Copyright 2026 Google LLC
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

//go:build integration_root

package integration

import (
	"bytes"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
	"golang.org/x/sys/unix"

	"github.com/google/abfs-casfs/api"
	"github.com/google/abfs-casfs/api/hash"
	"github.com/google/abfs-casfs/api/tapping"
	"github.com/google/abfs-casfs/api/xattr"
)

const (
	debugfsOverridePath = "/sys/kernel/debug/casfs/max_tap_entries_override"
	ChannelMultiplier   = 4
	MinimumChannel      = 64
)

var mntDirFlag = flag.String("mntDir", "", "Active CASFS directory path used to anchor test instances")

func TestKernelGuardRail_AbsoluteOverflowAsRoot(t *testing.T) {
	if !flag.Parsed() {
		flag.Parse()
	}

	if os.Getuid() != 0 {
		t.Skip("Skipping integration test: Must execute with root privileges (sudo)")
	}
	require.NoError(t, syscall.Setpgid(0, 0))

	if *mntDirFlag == "" {
		t.Fatal("Test configuration error: Missing mandatory -mntDir directory location flag reference")
	}

	// 1. Force global module allocation limits down to our micro-threshold (10 items)
	err := os.WriteFile(debugfsOverridePath, []byte("10\n"), 0200)
	require.NoError(t, err)

	// 2. Setup testing directories using the native api.FS paradigm
	workspace := filepath.Join(*mntDirFlag, "overflow_integration_sandbox")
	err = os.MkdirAll(workspace, 0755)
	require.NoError(t, err)
	defer os.RemoveAll(workspace)

	hasher := hash.NewHasher()
	fsEngine := xattr.NewCASFS(*mntDirFlag, hasher)

	ctrlDirName := ".ctrl"
	err = fsEngine.CreateControl(workspace, ctrlDirName)
	require.NoError(t, err)

	ctrlTapPath := filepath.Join(workspace, ctrlDirName, "tap")
	xattr.SetTapPath(fsEngine, ctrlTapPath)

	// 3. Prepare the payload execution command (a no-op 'true' pass)
	cmd := exec.Command("true")
	cmd.Dir = workspace

	// Use your exact production unix.Open call signature
	fd, err := unix.Open(ctrlTapPath, unix.O_RDWR|unix.O_TMPFILE|unix.O_EXCL|unix.O_CLOEXEC, 0600)
	require.NoError(t, err)
	defer unix.Close(fd)

	stdout := new(bytes.Buffer)
	stderr := new(bytes.Buffer)
	cmd.Stdout = stdout
	cmd.Stderr = stderr

	// Start the command process so the kernel session ties tracking events to it
	err = cmd.Start()
	require.NoError(t, err)

	// =========================================================================
	// STEP 1: FLOOD THE KERNEL POOL
	// We deliberately execute file actions while no Go reader is consuming.
	// 15 touches * 2 entries = 30 events stack up instantly inside kernel RAM,
	// forcing the memory ceiling tracker past the 10-item cap.
	// =========================================================================
	for i := 0; i < 15; i++ {
		tmpFile := filepath.Join(workspace, fmt.Sprintf("overflow_stress_%d.txt", i))
		_ = os.WriteFile(tmpFile, []byte("data"), 0644)
		_ = os.Remove(tmpFile)
	}

	// Give the kernel VFS thread a split second to finish scheduling async events
	time.Sleep(10 * time.Millisecond)

	// =========================================================================
	// STEP 2: RUN THE PRODUCTION READER PIPELINE
	// Spawning your exact production goroutine structure. When TapDecode executes,
	// it hits the kernel's .start() wrapper, which rejects the operation instantly.
	// =========================================================================
	var wg sync.WaitGroup
	var decodeErr error
	wg.Add(2)

	channelCapacity := runtime.NumCPU() * ChannelMultiplier
	if channelCapacity < MinimumChannel {
		channelCapacity = MinimumChannel
	}
	taps := make(chan *api.TapFSEntry, channelCapacity)
	tapReader := tapping.TapReaderCreate(fd)

	// Background thread #1: Decoding
	go func() {
		defer wg.Done()
		defer close(taps)
		decodeErr = tapping.TapDecode(tapReader, hasher, taps)
	}()

	// Background thread #2: Dispatching
	go func() {
		defer wg.Done()
		defer func() {
			for range taps {
			}
		}()
		for range taps {
			// Drain normally
		}
	}()

	// Wait for the monitored process to wrap up
	var exitErr *exec.ExitError
	err = cmd.Wait()
	if err != nil && !errors.As(err, &exitErr) {
		t.Fatalf("Command run broke unexpectedly: %v", err)
	}

	// Tell the kernel session that the workload is done
	err = tapReader.CmdDone()
	require.NoError(t, err)

	// Wait for background decoding loops to exit
	wg.Wait()

	// =========================================================================
	// STEP 3: NATIVE SYSTEM ERROR ASSERTION
	// The VFS layer translates -ENOSPC to "no space left on device" automatically.
	// =========================================================================
	require.Error(t, decodeErr, "Security breach: stream processed successfully despite buffer overflow")

	// Check for the native OS translation of syscall.ENOSPC
	require.Contains(t, decodeErr.Error(), "no space left on device",
		"Expected native Linux ENOSPC system error to terminate the stream")

	t.Logf("Integration Check Passed! Kernel dropped operations via native ENOSPC: %v", decodeErr)
}
