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

package tests

import (
	"bytes"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"reflect"
	"strings"

	"github.com/google/abfs-casfs/api"
	"github.com/google/abfs-casfs/api/xattr"
)

type tapTest struct {
	fs     api.FS
	hasher api.Hasher
	cli    string // Tap needs to run via it's executable to use unique pgid
}

func tapDataCompare(act, exp api.TapData) error {
	if !reflect.DeepEqual(act, exp) {
		fmt.Println("Got:")
		for _, r := range act.TapEntries {
			fmt.Printf("%v\n", r)
		}
		fmt.Println("Exp:")
		for _, r := range exp.TapEntries {
			fmt.Printf("%v\n", r)
		}

		return fmt.Errorf("Expected %v got %v\n", exp, act)
	}
	return nil

}
func (t *tapTest) RunCmd(workspace string, cmdArgs []string) (*api.TapCmdResult, error) {
	// Need to run via casfsapi tap to get the correct pgid
	// All tests here are non-resolve tests
	cmd := exec.Command(t.cli, "tap", "-n", "--")
	cmd.Args = append(cmd.Args, cmdArgs...)
	stdoutB := new(bytes.Buffer)
	stderrB := new(bytes.Buffer)
	cmd.Dir = workspace
	cmd.Stdout = stdoutB
	cmd.Stderr = stderrB
	err := cmd.Run()
	if err != nil {
		return nil, fmt.Errorf("tap error: %s (%s)", err, cmd.Stderr)
	}
	var result api.TapCmdResult
	err = result.UnmarshalJSON(stdoutB.Bytes(), t.hasher)
	if err != nil {
		return nil, fmt.Errorf("tap unmarshal error %s", err)
	}
	return &result, nil

}
func (t *tapTest) CtrlTapTest(root string) error {
	if err := MkdirTest(root); err != nil {
		return err
	}

	// tap is part of control
	dirName := ".ctrl"
	if err := t.fs.CreateControl(root, dirName); err != nil {
		return err
	}
	// TODO: (this is part of xattr implementation)
	xattr.SetTapPath(t.fs, filepath.Join(root, dirName, "tap"))
	return nil
}
func (t *tapTest) FileEncodingTest(mntRoot, workspace, fname string) error {
	err := t.CtrlTapTest(workspace)
	if err != nil {
		return err
	}

	tapResult, err := t.RunCmd(workspace, []string{"touch", fname})
	if err != nil {
		return err
	}
	tapEntryCount := len(tapResult.TapEntries)
	if tapEntryCount != 2 {
		return fmt.Errorf("invalid number of write patches : %d", tapEntryCount)
	}
	// Expect a write handle and a create
	expFile := path.Join(strings.TrimPrefix(workspace, mntRoot), fname)

	var exp api.TapData
	exp.TapEntries = []*api.TapFSEntry{
		{
			Type:    api.TapFSOpenWrite,
			Dirty:   false,
			Path:    expFile,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSAttrChange,
			Dirty:   false,
			Path:    expFile,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
	}
	return tapDataCompare(tapResult.TapData, exp)

}

func (t *tapTest) RenameTest(mntRoot, workspace, oldName, newName, expName string) error {
	err := t.CtrlTapTest(workspace)
	if err != nil {
		return err
	}

	cmd := exec.Command("/bin/sh", "-c", fmt.Sprintf("touch %s; mv %s %s", oldName, oldName, newName))
	cmd.Dir = workspace

	tapResult, err := t.fs.Tap(cmd)
	if err != nil {
		return err
	}

	expOldFile := path.Join(strings.TrimPrefix(workspace, mntRoot), oldName)
	expNewFile := fmt.Sprintf("%s/%s", path.Join(strings.TrimPrefix(workspace, mntRoot)), expName)

	var exp api.TapData
	exp.TapEntries = []*api.TapFSEntry{
		{
			Type:    api.TapFSOpenWrite,
			Dirty:   false,
			Path:    expOldFile,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSAttrChange,
			Dirty:   false,
			Path:    expOldFile,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSRename,
			Dirty:   false,
			Path:    expNewFile,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: expOldFile,
			Mode:    DefaultFileMode,
			Size:    0,
		},
	}
	return tapDataCompare(tapResult.TapData, exp)

}

func (t *tapTest) WriteRemoveTest(mntRoot, workspace string) error {

	err := t.CtrlTapTest(workspace)
	if err != nil {
		return err
	}

	fname := "test_file"
	cmd := exec.Command("/bin/sh", "-c", fmt.Sprintf("touch %s; rm %s", fname, fname))
	cmd.Dir = workspace

	tapResult, err := t.fs.Tap(cmd)
	if err != nil {
		return err
	}
	expFile := path.Join(strings.TrimPrefix(workspace, mntRoot), fname)
	var exp api.TapData
	exp.TapEntries = []*api.TapFSEntry{
		{
			Type:    api.TapFSOpenWrite,
			Dirty:   false,
			Path:    expFile,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSAttrChange,
			Dirty:   false,
			Path:    expFile,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSStat,
			Dirty:   false,
			Path:    expFile,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSUnlink,
			Dirty:   false,
			Path:    expFile,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
	}
	return tapDataCompare(tapResult.TapData, exp)
}

func (t *tapTest) HardLinkTest(mntRoot, workspace string) error {

	err := t.CtrlTapTest(workspace)
	if err != nil {
		return err
	}

	sCmd := fmt.Sprintf("touch file-1; cat file-1; ln file-1 file-2; rm file-1 file-2")
	cmd := exec.Command("/bin/sh", "-c", sCmd)
	cmd.Dir = workspace

	tapResult, err := t.fs.Tap(cmd)
	if err != nil {
		return err
	}
	expFile1 := path.Join(strings.TrimPrefix(workspace, mntRoot), "file-1")
	expFile2 := path.Join(strings.TrimPrefix(workspace, mntRoot), "file-2")

	var exp api.TapData
	exp.TapEntries = []*api.TapFSEntry{
		{
			Type:    api.TapFSOpenWrite,
			Dirty:   false,
			Path:    expFile1,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSLink,
			Dirty:   false,
			Path:    expFile2,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: expFile1,
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSStat,
			Dirty:   false,
			Path:    expFile1,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSUnlink,
			Dirty:   false,
			Path:    expFile1,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSStat,
			Dirty:   false,
			Path:    expFile2,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSUnlink,
			Dirty:   false,
			Path:    expFile2,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
	}
	if err := tapDataCompare(tapResult.TapData, exp); err != nil {
		return err
	}
	if err := os.WriteFile(path.Join(workspace, "file-1"), []byte(""), 0644); err != nil {
		return err
	}

	sCmd = fmt.Sprintf("cat file-1; ln file-1 file-2; rm file-1 file-2")
	cmd = exec.Command("/bin/sh", "-c", sCmd)
	cmd.Dir = workspace

	tapResult, err = t.fs.Tap(cmd)
	if err != nil {
		return err
	}
	exp.TapEntries = []*api.TapFSEntry{
		{
			Type:    api.TapFSOpenRead,
			Dirty:   false,
			Path:    expFile1,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSStat,
			Dirty:   false,
			Path:    expFile1,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSLink,
			Dirty:   false,
			Path:    expFile2,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: expFile1,
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSStat,
			Dirty:   false,
			Path:    expFile1,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSUnlink,
			Dirty:   false,
			Path:    expFile1,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSStat,
			Dirty:   false,
			Path:    expFile2,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSUnlink,
			Dirty:   false,
			Path:    expFile2,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
	}

	return tapDataCompare(tapResult.TapData, exp)
}

func (t *tapTest) MkdirTest(mntRoot, workspace string) error {
	err := t.CtrlTapTest(workspace)
	if err != nil {
		return err
	}

	sCmd := fmt.Sprintf("mkdir  d1")
	cmd := exec.Command("/bin/sh", "-c", sCmd)
	cmd.Dir = workspace

	tapResult, err := t.fs.Tap(cmd)
	if err != nil {
		return err
	}
	expName := path.Join(strings.TrimPrefix(workspace, mntRoot), "d1")
	var exp api.TapData
	exp.TapEntries = []*api.TapFSEntry{
		{
			Type:    api.TapFSMkdir,
			Path:    expName,
			Hash:    t.hasher.EmptyDirHash(),
			AuxPath: "",
			Mode:    DefaultDirMode,
			Size:    0,
		},
	}
	return tapDataCompare(tapResult.TapData, exp)
}
func (t *tapTest) RmdirTest(mntRoot, workspace string) error {
	err := t.CtrlTapTest(workspace)
	if err != nil {
		return err
	}

	sCmd := fmt.Sprintf("mkdir d1;rmdir d1")
	cmd := exec.Command("/bin/sh", "-c", sCmd)
	cmd.Dir = workspace

	tapResult, err := t.fs.Tap(cmd)
	if err != nil {
		return err
	}
	expName := path.Join(strings.TrimPrefix(workspace, mntRoot), "d1")
	var exp api.TapData
	exp.TapEntries = []*api.TapFSEntry{
		{
			Type:    api.TapFSMkdir,
			Dirty:   false,
			Path:    expName,
			Hash:    t.hasher.EmptyDirHash(),
			AuxPath: "",
			Mode:    DefaultDirMode,
			Size:    0,
		},
		{
			Type:    api.TapFSUnlink,
			Dirty:   false,
			Path:    expName,
			Hash:    t.hasher.EmptyDirHash(),
			AuxPath: "",
			Mode:    DefaultDirMode,
			Size:    0,
		},
	}
	return tapDataCompare(tapResult.TapData, exp)
}
func (t *tapTest) SymlinkDirTest(mntRoot, workspace string) error {
	err := t.CtrlTapTest(workspace)
	if err != nil {
		return err
	}

	sCmd := fmt.Sprintf("mkdir d1;ln -s d1 s1")
	cmd := exec.Command("/bin/sh", "-c", sCmd)
	cmd.Dir = workspace

	tapResult, err := t.fs.Tap(cmd)
	if err != nil {
		return err
	}

	// Don't care about the hash value of the symlink
	// So override the value with whatever is returned from the tap
	tapEntryCount := len(tapResult.TapEntries)
	if tapEntryCount != 2 {
		return fmt.Errorf("expected %d entries got %d\n", 2, tapEntryCount)
	}

	expName := path.Join(strings.TrimPrefix(workspace, mntRoot), "d1")
	expLinkName := path.Join(strings.TrimPrefix(workspace, mntRoot), "s1")
	var exp api.TapData
	exp.TapEntries = []*api.TapFSEntry{
		{
			Type:    api.TapFSMkdir,
			Dirty:   false,
			Path:    expName,
			Hash:    t.hasher.EmptyDirHash(),
			AuxPath: "",
			Mode:    DefaultDirMode,
			Size:    0,
		},
		{
			Type:    api.TapFSSymlink,
			Dirty:   false,
			Path:    expLinkName,
			Hash:    tapResult.TapEntries[1].Hash, // Copy hash from tap data
			AuxPath: "",
			Mode:    DefaultSymlinkMode,
			Size:    int64(len("d1")),
		},
	}
	return tapDataCompare(tapResult.TapData, exp)
}
func (t *tapTest) SymlinkFileTest(mntRoot, workspace string) error {
	err := t.CtrlTapTest(workspace)
	if err != nil {
		return err
	}
	srcName := "source_file_name_with_ext.data"
	linkName := "link"
	sCmd := fmt.Sprintf("touch %s;ln -s %s %s", srcName, srcName, linkName)
	cmd := exec.Command("/bin/sh", "-c", sCmd)
	cmd.Dir = workspace

	tapResult, err := t.fs.Tap(cmd)
	if err != nil {
		return err
	}

	// Don't care about the hash value of the symlink
	// So override the value with whatever is returned from the tap
	tapEntryCount := len(tapResult.TapEntries)
	if tapEntryCount != 3 {
		return fmt.Errorf("expected %d entries got %d\n", 3, tapEntryCount)
	}

	expName := path.Join(strings.TrimPrefix(workspace, mntRoot), srcName)
	expLinkName := path.Join(strings.TrimPrefix(workspace, mntRoot), linkName)
	var exp api.TapData
	exp.TapEntries = []*api.TapFSEntry{
		{
			Type:    api.TapFSOpenWrite,
			Dirty:   false,
			Path:    expName,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSAttrChange,
			Dirty:   false,
			Path:    expName,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSSymlink,
			Dirty:   false,
			Path:    expLinkName,
			Hash:    tapResult.TapEntries[2].Hash, // Copy hash from tap data
			AuxPath: "",
			Mode:    DefaultSymlinkMode,
			Size:    int64(len(srcName)),
		},
	}
	return tapDataCompare(tapResult.TapData, exp)
}

// Test that a readonly create file shows up in write patch
func (t *tapTest) ReadOnlyCreateTest(mntRoot, workspace string) error {
	err := t.CtrlTapTest(workspace)
	if err != nil {
		return err
	}
	filename := filepath.Join(workspace, "readonly_file")
	wd, err := os.Getwd()
	if err != nil {
		return err
	}
	app := filepath.Join(wd, "ronly/ronly.go")
	tapResult, err := t.RunCmd(workspace, []string{"go", "run", app, filename})
	if err != nil {
		return err
	}

	expName := path.Join(strings.TrimPrefix(workspace, mntRoot), "readonly_file")
	var exp api.TapData

	exp.TapEntries = []*api.TapFSEntry{
		{
			Type:    api.TapFSOpenWrite,
			Dirty:   false,
			Path:    expName,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    fs.FileMode(0444),
			Size:    0,
		},
	}

	return tapDataCompare(tapResult.TapData, exp)
}

// Test that a readonly create file shows up in write patch
func (t *tapTest) ReadOnlyCreateExecTest(mntRoot, workspace string) error {
	err := t.CtrlTapTest(workspace)
	if err != nil {
		return err
	}

	filename := "readonly_file"
	wd, err := os.Getwd()
	if err != nil {
		return err
	}

	app := filepath.Join(wd, "ronly/ronly.go")
	tapResult, err := t.RunCmd(workspace, []string{"go", "run", app, filename, "exc"})
	if err != nil {
		return err
	}
	expName := path.Join(strings.TrimPrefix(workspace, mntRoot), filename)
	var exp api.TapData

	exp.TapEntries = []*api.TapFSEntry{
		{
			Type:    api.TapFSOpenWrite,
			Dirty:   false,
			Path:    expName,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    fs.FileMode(0555),
			Size:    0,
		},
	}

	return tapDataCompare(tapResult.TapData, exp)
}

// Test that a readonly create file shows up in write entry
func (t *tapTest) ChmodTest(mntRoot, workspace string) error {
	fname := "test_file"
	if err := t.FileEncodingTest(mntRoot, workspace, fname); err != nil {
		return err
	}
	tapResult, err := t.RunCmd(workspace, []string{"chmod", "755", fname})
	if err != nil {
		return err
	}
	expFile := path.Join(strings.TrimPrefix(workspace, mntRoot), fname)
	var exp api.TapData
	exp.TapEntries = []*api.TapFSEntry{
		{
			Type:    api.TapFSStat,
			Dirty:   false,
			Path:    expFile,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    DefaultFileMode,
			Size:    0,
		},
		{
			Type:    api.TapFSAttrChange,
			Dirty:   false,
			Path:    expFile,
			Hash:    t.hasher.EmptyFileHash(),
			AuxPath: "",
			Mode:    fs.FileMode(0755),
			Size:    0,
		},
	}
	return tapDataCompare(tapResult.TapData, exp)
}

// TestBatchDrainingAndOverflow verifies that the seq_file iteration engine can
// successfully cross internal buffer block boundaries, handle transient empty queues,
// and cleanly close out the tracking session on ioctl execution without dropped frames.
func (t *tapTest) TestBatchDrainingAndOverflow(mntRoot, workspace string) error {
	if err := t.CtrlTapTest(workspace); err != nil {
		return err
	}

	// 1. Generate a massive multi-staged command pipeline string to aggressively
	// stress the seq_file page buffer limits. We want to cross the standard 4KB page ceiling.
	var sb strings.Builder
	fileCount := 120
	for i := 0; i < fileCount; i++ {
		// Use highly expressive long path names to force high dynamic byte footprints
		sb.WriteString(fmt.Sprintf("touch detailed_trace_file_marker_index_identifier_%d.log; ", i))
	}
	for i := 0; i < fileCount; i++ {
		sb.WriteString(fmt.Sprintf("rm detailed_trace_file_marker_index_identifier_%d.log; ", i))
	}

	cmdArgs := []string{"/bin/sh", "-c", sb.String()}

	// 2. Invoke the tracking command path. Under the hood, this uses your modified Go reader
	// execution block to issue unix.Read loop passes and the final CmdDone() ioctl handshake.
	tapResult, err := t.RunCmd(workspace, cmdArgs)
	if err != nil {
		return fmt.Errorf("stress batch runner failed: %w", err)
	}

	// Each successful 'touch' produces 2 structural entry logs (OpenWrite + AttrChange).
	// Each successful 'rm' produces 2 structural entry logs (Stat + Unlink).
	// Total expected entry count calculation: (120 * 2) + (120 * 2) = 480 entries.
	expectedTotalEntries := fileCount * 4
	actualEntryCount := len(tapResult.TapEntries)

	if actualEntryCount != expectedTotalEntries {
		return fmt.Errorf("data loss or truncation detected! Expected exactly %d trace entries across page boundaries, but got %d",
			expectedTotalEntries, actualEntryCount)
	}

	// 3. Verify data structural integrity (ensuring no partial allocations or corruption)
	for i, entry := range tapResult.TapEntries {
		if entry.Path == "" {
			return fmt.Errorf("corrupted record tracking block detected at layout index %d (empty string variant path)", i)
		}
		if entry.Type == 0 {
			return fmt.Errorf("invalid trace operation type evaluated on file %s at offset %d", entry.Path, i)
		}
	}

	return nil
}
func (t *tapTest) ARITapReadTest(mntRoot, workspace, ari string, payload []byte) error {
	if err := t.CtrlTapTest(workspace); err != nil {
		return err
	}

	fname := "ari_tapped_file.dat"
	fpath := filepath.Join(workspace, fname)

	// 1. Create target empty file
	f, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return fmt.Errorf("failed to create file: %w", err)
	}
	f.Close()

	// 2. Set ARI and size on the target node
	if err := t.fs.SetARISize(fpath, ari, int64(len(payload))); err != nil {
		return fmt.Errorf("SetARISize failed: %w", err)
	}

	// 3. Execute a command under tap that reads the ARI-backed file
	sCmd := fmt.Sprintf("cat %s > /dev/null", fname)
	cmd := exec.Command("/bin/sh", "-c", sCmd)
	cmd.Dir = workspace

	tapResult, err := t.fs.Tap(cmd)
	if err != nil {
		return fmt.Errorf("tap execution failed: %w", err)
	}

	expFile := path.Join(strings.TrimPrefix(workspace, mntRoot), fname)

	// 4. Validate that the tap entry carries IsARI = true and the matching ARI string
	var foundARIRead bool
	for _, entry := range tapResult.TapEntries {
		if entry.Path == expFile && entry.Type == api.TapFSOpenRead {
			foundARIRead = true
			if !entry.IsARI {
				return fmt.Errorf("expected tap entry IsARI to be true, got false")
			}
			if entry.ARI != ari {
				return fmt.Errorf("expected tap ARI %q, got %q", ari, entry.ARI)
			}
			if entry.Size != int64(len(payload)) {
				return fmt.Errorf("expected tap size %d, got %d", len(payload), entry.Size)
			}
		}
	}

	if !foundARIRead {
		return fmt.Errorf("did not find expected TapFSOpenRead entry for ARI file %s", expFile)
	}

	return nil
}
