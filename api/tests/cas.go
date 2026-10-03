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
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/google/abfs-casfs/api"
	"golang.org/x/sys/unix"
)

type casTest struct {
	fs     api.FS
	hasher api.Hasher
}

func (c *casTest) MkdirTest(path string) error {
	if err := MkdirTest(path); err != nil {
		return err
	}
	hash, err := c.fs.Hash(path)
	if err != nil {
		return err
	}

	if !hash.IsEmptyDir() {
		return fmt.Errorf("did not get empty dir hash on mkdir %s", hash)
	}
	dirty, err := c.fs.Dirty(path)
	if err != nil {
		return err
	}
	if dirty {
		return fmt.Errorf("directory dirty")
	}
	return nil

}
func (c *casTest) MkdirTestStat(path string) error {

	if err := MkdirTest(path); err != nil {
		return err
	}
	hs, err := c.fs.HashStat(path)
	if err != nil {
		return err
	}
	if !hs.Hash.IsEmptyDir() {
		return fmt.Errorf("did not get empty dir hash on mkdir %s", hs.Hash)
	}
	if !hs.Mode.IsDir() {
		return fmt.Errorf("expected directory mode got %o", hs.Mode)
	}
	if hs.Size != 0 {
		return fmt.Errorf("wrong size on stat %d", hs.Size)
	}
	return nil

}
func (c *casTest) FileTest(path string) error {
	if err := FileTest(path); err != nil {
		return err
	}
	hash, err := c.fs.Hash(path)
	if err != nil {
		return err
	}
	if !hash.IsEmptyFile() {
		return fmt.Errorf("did not get empty file hash")
	}

	hash, sz, err := c.fs.HashSize(path)
	if !hash.IsEmptyFile() {
		return fmt.Errorf("did not get empty file hash via hash size")
	}
	if sz != 0 {
		return fmt.Errorf("did not get 0 size")
	}

	parent := filepath.Dir(path)
	dirty, err := c.fs.Dirty(parent)
	if err != nil {
		return err
	}
	if !dirty {
		return fmt.Errorf("parent directory not dirty")
	}
	return nil
}
func (c *casTest) FileTestContent(path, content string) error {
	if err := FileTestContent(path, content); err != nil {
		return err
	}
	hash, err := c.fs.Hash(path)
	if err != nil {
		return err
	}
	if hash.IsEmptyFile() {
		return fmt.Errorf("got empty file hash")
	}

	hash, sz, err := c.fs.HashSize(path)
	if hash.IsEmptyFile() {
		return fmt.Errorf("got empty file hash via hash size")
	}
	if sz != int64(len(content)) {
		return fmt.Errorf("got %d size via hash size expected %d", sz, len(content))
	}

	return nil
}

func (c *casTest) FileTestContentStat(path, content string) error {

	if err := FileTestContent(path, content); err != nil {
		return err
	}
	hs, err := c.fs.HashStat(path)
	if err != nil {
		return err
	}
	if hs.Hash.IsEmptyFile() {
		return fmt.Errorf("got empty file hash")
	}

	if hs.Size != int64(len(content)) {
		return fmt.Errorf("got %d size via hash stat expected %d", hs.Size, len(content))
	}
	if !hs.Mode.IsRegular() {
		return fmt.Errorf("expected regular file got %o", hs.Mode)
	}
	return nil

}
func (c *casTest) FileTestTruncate(path, content string) error {
	if err := FileTestTruncate(path, content); err != nil {
		return err
	}
	hash, err := c.fs.Hash(path)
	if err != nil {
		return err
	}
	if !hash.IsEmptyFile() {
		return fmt.Errorf("got non-empty file hash")
	}
	return nil
}

func (c *casTest) FileTestTruncateOpen(path, content string) error {
	if err := FileTestTruncateOpen(path, content); err != nil {
		return err
	}
	hash, err := c.fs.Hash(path)
	if err != nil {
		return err
	}
	if !hash.IsEmptyFile() {
		return fmt.Errorf("got non-empty file hash")
	}
	return nil
}

func (c *casTest) FileTestCopy(path, cpPath, content string) error {
	if err := FileTestContent(path, content); err != nil {
		return err
	}
	hash, sz, err := c.fs.HashSize(path)
	if err != nil {
		return err
	}
	osMode := 0644
	if err := syscall.Mknod(cpPath, syscall.S_IFREG|uint32(osMode), 0); err != nil {
		return err
	}
	if err := c.fs.SetHashSize(cpPath, hash, sz); err != nil {
		return err
	}

	cpHash, err := c.fs.Hash(cpPath)
	if err != nil {
		return err
	}
	if !hash.Equals(cpHash) {
		return fmt.Errorf("hash mismatch %s %s", hash, cpHash)
	}
	if ValidateFileContent(cpPath, content) != nil {
		return err
	}
	return nil
}

func (c *casTest) SymlinkTest(target, symlink string) error {
	err := SymlinkTest(target, symlink)
	if err != nil {
		return err
	}
	hash, err := c.fs.Hash(target)
	if err != nil {
		return err
	}
	symHash, err := c.fs.Hash(symlink)
	if err != nil {
		return err
	}

	if hash.Equals(symHash) {
		return fmt.Errorf("file and symlink have same hash %s", hash)
	}
	parent := filepath.Dir(target)
	dirty, err := c.fs.Dirty(parent)
	if err != nil {
		return err
	}
	if !dirty {
		return fmt.Errorf("parent directory not dirty")
	}

	return nil
}

func (c *casTest) SymlinkTestStat(target, symlink string) error {

	err := SymlinkTest(target, symlink)
	if err != nil {
		return err
	}
	hs, err := c.fs.HashStat(target)
	if err != nil {
		return err
	}
	sh, err := c.fs.HashStat(symlink)
	if err != nil {
		return err
	}

	if hs.Hash.Equals(sh.Hash) {
		return fmt.Errorf("file and symlink have same hash %s", hs.Hash)
	}
	if (sh.Mode & fs.ModeSymlink) == 0 {
		return fmt.Errorf("expected symlink got %o", sh.Mode)
	}
	if sh.Size != int64(len(target)) {
		return fmt.Errorf("got %d len via hash stat expected %d", sh.Size, len(target))
	}
	return nil
}
func (c *casTest) SymlinkTestNoSourceStat(target, symlink string) error {
	err := SymlinkTestNoSource(target, symlink)
	if err != nil {
		return err
	}
	sh, err := c.fs.HashStat(symlink)
	if err != nil {
		return err
	}

	if (sh.Mode & fs.ModeSymlink) == 0 {
		return fmt.Errorf("expected symlink got %o", sh.Mode)
	}
	if sh.Size != int64(len(target)) {
		return fmt.Errorf("got %d len via hash stat expected %d", sh.Size, len(target))
	}

	return nil
}
func (c *casTest) HardLinkFile(target, link string) error {
	err := HardLinkFile(target, link)
	if err != nil {
		return err
	}
	// New file is should be dirty check via link
	dirty, err := c.fs.Dirty(link)
	if err != nil {
		return err
	}
	if !dirty {
		return fmt.Errorf("link file <%s> is not dirty", link)
	}
	// Snapshot the file via link make sure target is clean
	h1, err := c.fs.Hash(link)
	if err != nil {
		return err
	}
	dirty, err = c.fs.Dirty(target)
	if dirty {
		return fmt.Errorf("target <%s> is still dirty after hashing via link", target)
	}

	h2, err := c.fs.Hash(target)
	if err != nil {
		return err
	}
	if !h1.Equals(h2) {
		return fmt.Errorf("target <%s> and hard link <%s> have different hashes", target, link)
	}
	return nil
}
func (c *casTest) DirTestStage(path string) error {
	if err := DirTestStage(path); err != nil {
		return err
	}

	hash, err := c.fs.Hash(path)
	if err != nil {
		return err
	}
	if hash.IsEmptyDir() {
		return fmt.Errorf("got empty dir hash")
	}
	return nil
}

func (c *casTest) DirTestStageCopy(path, cpPath string) error {
	if err := DirTestStage(path); err != nil {
		return err
	}
	hash, err := c.fs.Hash(path)
	if err != nil {
		return err
	}
	if err := MkdirTest(cpPath); err != nil {
		return err
	}
	if err := c.fs.SetHash(cpPath, hash); err != nil {
		return err
	}
	cpHash, err := c.fs.Hash(cpPath)
	if err != nil {
		return err
	}
	if !hash.Equals(cpHash) {
		return fmt.Errorf("hash mismatch %s %s", hash, cpHash)
	}
	if err := ValidateTestStage(cpPath); err != nil {
		return err
	}

	return nil
}

// Check an excluded child does not change the parent until
// set to included
func (c *casTest) checkExcluded(startHash api.Hash, parent, child string) error {
	if err := c.fs.SetExcluded(child, true); err != nil {
		return err
	}
	hash1, err := c.fs.Hash(parent)
	if err != nil {
		return err
	}
	if !startHash.Equals(hash1) {
		return fmt.Errorf("hashes are not the same")
	}
	if exc, err := c.fs.Excluded(child); err != nil {
		return err
	} else if exc != true {
		return fmt.Errorf("expected excluded but got included")
	}
	if err := c.fs.SetExcluded(child, false); err != nil {
		return err
	}
	if exc, err := c.fs.Excluded(child); err != nil {
		return err
	} else if exc != false {
		return fmt.Errorf("expected included but got excluded")
	}

	hash2, err := c.fs.Hash(parent)
	if err != nil {
		return err
	}
	if startHash.Equals(hash2) {
		return fmt.Errorf("hashes are the same")
	}
	return nil
}
func (c *casTest) ExcludeDirTest(parent, dir string) error {
	// grab hash of parent directory
	// create new directory and exclude it
	// grab hash of parent directory make sure still same
	// unexclude new directory
	// grab hash of parent directory make sure changed
	if err := c.MkdirTest(parent); err != nil {
		return err
	}
	hash, err := c.fs.Hash(parent)
	if err != nil {
		return err
	}
	dirPath := filepath.Join(parent, dir)

	if err := c.MkdirTest(dirPath); err != nil {
		return err
	}
	return c.checkExcluded(hash, parent, dirPath)

}

func (c *casTest) ExcludeFileTest(parent, fname string) error {
	// same as exclude dir but use a file instead
	if err := c.MkdirTest(parent); err != nil {
		return err
	}
	hash, err := c.fs.Hash(parent)
	if err != nil {
		return err
	}
	fPath := filepath.Join(parent, fname)

	if err := c.FileTest(fPath); err != nil {
		return err
	}
	return c.checkExcluded(hash, parent, fPath)

}
func (c *casTest) DebugTests(dir string) error {
	if err := c.MkdirTest(dir); err != nil {
		return err
	}
	if exc, err := c.fs.Debug(dir); err != nil {
		return err
	} else if exc != false {
		return fmt.Errorf("expected non-debug but got debug")
	}
	if err := c.fs.SetDebug(dir, true); err != nil {
		return err
	}
	if exc, err := c.fs.Debug(dir); err != nil {
		return err
	} else if exc != true {
		return fmt.Errorf("expected debug got non-debug")
	}
	if err := c.fs.SetDebug(dir, false); err != nil {
		return err
	}

	if exc, err := c.fs.Debug(dir); err != nil {
		return err
	} else if exc != false {
		return fmt.Errorf("expected non-debug but got debug")
	}
	return nil
}

// Hash coverage tests
func (c *casTest) HashTests() error {
	// Expect unique hashes for empty dir, file, and zero
	zero := c.hasher.ZeroHash()
	emptyDir := c.hasher.EmptyDirHash()
	emptyFile := c.hasher.EmptyFileHash()
	_ = c.hasher.DescriptionOfHashString()

	if emptyDir.IsZero() {
		return fmt.Errorf("empty dir hash is zero")
	}
	if emptyFile.IsZero() {
		return fmt.Errorf("empty file hash is zero")
	}
	if !zero.IsZero() {
		return fmt.Errorf("zero hash is not zero")
	}

	if emptyDir.Equals(emptyFile) {
		return fmt.Errorf("empty dir hash equals empty file hash")
	}

	var buf bytes.Buffer
	n, err := emptyFile.Write(&buf)

	if err != nil {
		return err
	}

	newHash := c.hasher.ZeroHash()
	n2, err := newHash.Read(&buf)
	if err != nil {
		return fmt.Errorf("failed to read hash")
	}
	if n != n2 {
		return fmt.Errorf("wrote %d bytes read %d", n, n2)
	}
	if !emptyFile.Equals(newHash) {
		return fmt.Errorf("reading hash via io returned unexpected hash")
	}
	// Create junk hash
	if _, err := c.hasher.NewHash("notahash"); err == nil {
		return fmt.Errorf("expected error on bad hash value got success")
	}

	if _, err := c.hasher.NewHash(strings.Repeat("Z", n*2)); err == nil {
		return fmt.Errorf("expected error on bad hash value got success")
	}
	return nil
}

// SnapshotMmapWriteBlockTest verifies that attempting to generate a snapshot/hash
// on an active shared-writable memory mapping fails cleanly with an EBUSY error status,
// and passes cleanly after the memory mapping is unmapped.
func (c *casTest) SnapshotMmapWriteBlockTest(fpath string) error {
	// 1. Create the target file and allocate an initial data payload
	// (VFS memory maps require an existing file size footprint to bind pages against)
	f, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return fmt.Errorf("failed to create test file: %w", err)
	}
	defer f.Close()

	initialData := []byte("baseline data footprint for page cache mapping alignment matrix")
	if _, err := f.Write(initialData); err != nil {
		return fmt.Errorf("failed to write file baseline payload: %w", err)
	}

	// 2. Establish a live shared-writable mmap session against the file descriptor
	fd := int(f.Fd())
	pageSize := os.Getpagesize()
	mmapData, err := unix.Mmap(fd, 0, pageSize, unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED)
	if err != nil {
		return fmt.Errorf("unix.Mmap initialization failed: %w", err)
	}

	// Dynamic emergency unmap coordinator if an unexpected assertion throws early
	isUnmapped := false
	defer func() {
		if !isUnmapped {
			unix.Munmap(mmapData)
		}
	}()

	// Dirty a byte in the memory mapping layer to simulate an in-flight write op
	mmapData[0] = 'W'

	// 3. Attempt to trigger a snapshot/hash calculation while the shared-writable VMA mapping is live
	_, err = c.fs.Hash(fpath)

	// ASSERTION 1: The engine must block the snapshot request and fail
	if err == nil {
		return fmt.Errorf("vulnerability: snapshot/hash evaluation succeeded while file was actively mapped shared-writable")
	}

	// ASSERTION 2: Verify the returned error maps to a busy resource state (-EBUSY)
	if !strings.Contains(strings.ToLower(err.Error()), "busy") {
		return fmt.Errorf("expected failure state EBUSY (resource busy), but got alternative error: %v", err)
	}

	// 4. Tear down the memory mapping layer cleanly
	if err := unix.Munmap(mmapData); err != nil {
		return fmt.Errorf("failed to release mmap memory allocation: %w", err)
	}
	isUnmapped = true

	// 5. Re-attempt the snapshot now that the shared-writable block condition is completely clear
	_, err = c.fs.Hash(fpath)
	if err != nil {
		return fmt.Errorf("snapshot failed to execute after memory map was cleared: %w", err)
	}

	return nil
}

// SeekReadPositionConsistencyTest verifies that performing a Seek followed by partial
// sequential reads updates the file position descriptor symmetrically on the frontend filesystem.
func (c *casTest) SeekReadPositionConsistencyTest(fpath string) error {
	// 1. Create a file with known patterned segments
	f, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return fmt.Errorf("failed to create check file: %w", err)
	}
	defer f.Close()

	payload := []byte("abcdefghijklmnopqrstuvwxyz0123456789")
	if _, err := f.Write(payload); err != nil {
		return fmt.Errorf("failed to populate patterned payload: %w", err)
	}
	f.Close()

	// 2. Reopen the file to establish a clean file descriptor tracking position
	f, err = os.OpenFile(fpath, os.O_RDONLY, 0)
	if err != nil {
		return fmt.Errorf("failed to reopen file for seek testing: %w", err)
	}
	defer f.Close()

	// 3. Seek to an explicit middle offset (e.g., index 10 -> 'k')
	targetOffset := int64(10)
	pos, err := f.Seek(targetOffset, io.SeekStart)
	if err != nil {
		return fmt.Errorf("initial Seek failed: %w", err)
	}
	if pos != targetOffset {
		return fmt.Errorf("expected position return %d, got %d", targetOffset, pos)
	}

	// 4. Perform a partial read loop (e.g., 5 bytes)
	buf := make([]byte, 5)
	n, err := f.Read(buf)
	if err != nil {
		return fmt.Errorf("partial read after seek failed: %w", err)
	}
	if n != 5 {
		return fmt.Errorf("expected to read 5 bytes, got %d", n)
	}
	if string(buf) != "klmno" {
		return fmt.Errorf("data mismatch after seek! expected 'klmno', got %q", string(buf))
	}

	// 5. Query the current position via Seek relative to current track
	currentPos, err := f.Seek(0, io.SeekCurrent)
	if err != nil {
		return fmt.Errorf("querying seek location failed: %w", err)
	}

	// CRITICAL ASSERTION: The file pointer must have advanced by the read size (10 + 5 = 15)
	expectedPos := targetOffset + int64(n)
	if currentPos != expectedPos {
		return fmt.Errorf("VFS f_pos desynchronization detected! outer fd reported position %d, but expected %d", currentPos, expectedPos)
	}

	return nil
}

// AtomicAppendWriteTest verifies that opening a file with O_APPEND correctly respects
// file end boundaries on multi-threaded or sequential discrete write invocations.
func (c *casTest) AtomicAppendWriteTest(fpath string) error {
	// 1. Write an initial block
	f, err := os.OpenFile(fpath, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return fmt.Errorf("failed to initialize append target: %w", err)
	}
	if _, err := f.Write([]byte("AAA")); err != nil {
		f.Close()
		return err
	}
	f.Close()

	// 2. Open with explicit O_APPEND flag constraint
	f, err = os.OpenFile(fpath, os.O_WRONLY|os.O_APPEND, 0644)
	if err != nil {
		return fmt.Errorf("failed to open file with O_APPEND: %w", err)
	}

	// Write a second block
	if _, err := f.Write([]byte("BBB")); err != nil {
		f.Close()
		return err
	}
	f.Close()

	// 3. Read back and validate structure sequencing
	content, err := os.ReadFile(fpath)
	if err != nil {
		return fmt.Errorf("failed to verify final payload file: %w", err)
	}

	if string(content) != "AAABBB" {
		return fmt.Errorf("O_APPEND flag was stripped or ignored! Expected 'AAABBB', but file content is %q", string(content))
	}

	return nil
}

// ConcurrentSharedDescriptorStressTest forces multiple threads to read and write
// simultaneously using the exact same file descriptor instance.
func (c *casTest) ConcurrentSharedDescriptorStressTest(fpath string) error {
	f, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return err
	}
	defer f.Close()

	// Pre-populate payload
	baseData := []byte(strings.Repeat("A", 4096))
	if _, err := f.Write(baseData); err != nil {
		return err
	}

	var wg sync.WaitGroup
	workers := 10
	iterations := 100

	// Launch concurrent readers and writers on the SAME file descriptor 'f'
	for i := 0; i < workers; i++ {
		wg.Add(2)
		// Reader loop
		go func() {
			defer wg.Done()
			buf := make([]byte, 64)
			for j := 0; j < iterations; j++ {
				_, _ = f.ReadAt(buf, int64((j*64)%4000))
			}
		}()
		// Writer loop (forces state promotions from RO to RW scratch contexts)
		go func(workerID int) {
			defer wg.Done()
			updateBlock := []byte(strings.Repeat(fmt.Sprintf("%d", workerID%10), 64))
			for j := 0; j < iterations; j++ {
				_, _ = f.WriteAt(updateBlock, int64((j*64)%4000))
			}
		}(i)
	}

	wg.Wait()
	return nil
}

// TruncateReadInterleaveTest validates that page cache state maps drop cleanly
// during runtime file shrinking loops.
func (c *casTest) TruncateReadInterleaveTest(fpath string) error {
	f, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return err
	}
	defer f.Close()

	// 1. Fill with a large data chunk
	bigData := []byte(strings.Repeat("X", 8192))
	if _, err := f.Write(bigData); err != nil {
		return err
	}

	// 2. Open an independent reader handle to capture page maps
	reader, err := os.OpenFile(fpath, os.O_RDONLY, 0)
	if err != nil {
		return err
	}
	defer reader.Close()

	buf := make([]byte, 10)
	if _, err := reader.ReadAt(buf, 0); err != nil {
		return err
	}

	// 3. Truncate the file through the primary write handle mid-flight
	if err := f.Truncate(1024); err != nil {
		return err
	}

	// 4. Attempt to read out past the new truncated size limit on the reader handle
	_, err = reader.ReadAt(buf, 2048)
	// CRITICAL ASSERTION: Must return an explicit EOF error.
	// If it successfully returns 'XXXX', your page cache invalidation failed!
	if err == nil {
		return fmt.Errorf("page cache leak! Able to read data past truncated boundary")
	}

	return nil
}

// IdempotentCollisionDoubleFreeTest verifies that sequential snapshot evaluations
// of identical files safely break out of backend rename passes without a double-free panic.
func (c *casTest) IdempotentCollisionDoubleFreeTest(p1, p2 string) error {
	payload := []byte("identical data chunk payload block for duplicate hash matching")

	// Write identical data via the mount view
	if err := os.WriteFile(p1, payload, 0644); err != nil {
		return err
	}
	if err := os.WriteFile(p2, payload, 0644); err != nil {
		return err
	}

	// 1. Snapshot the first file to establish the immutable backing block
	_, err := c.fs.Hash(p1)
	if err != nil {
		return fmt.Errorf("initial snapshot assignment failed: %w", err)
	}

	// 2. Snapshot the second file. This forces a hash collision (-EEXIST) on the rename pass.
	// If the driver double-frees the scratch reference counts, this call will panic or crash the module.
	_, err = c.fs.Hash(p2)
	if err != nil {
		return fmt.Errorf("idempotent collision handling failed: %w", err)
	}

	return nil
}

// ConcurrencyNamespaceRaceTest builds parallel read-intent snapshot loops on distinct files
// to confirm the VFS modification hooks update namespaces under safe lock bounds.
func (c *casTest) ConcurrencyNamespaceRaceTest(testDir string) error {
	const workerCount = 15
	var wg sync.WaitGroup
	errChan := make(chan error, workerCount*3)

	// Pre-allocate distinct files via the mount view
	files := make([]string, workerCount)
	for i := 0; i < workerCount; i++ {
		files[i] = filepath.Join(testDir, fmt.Sprintf("concurrency_race_%d.txt", i))
		if err := os.WriteFile(files[i], []byte(fmt.Sprintf("stable thread payload block %d", i)), 0644); err != nil {
			return err
		}
	}

	// Fire concurrent snapshot loops simultaneously
	for i := 0; i < workerCount; i++ {
		wg.Add(1)
		go func(fpath string) {
			defer wg.Done()
			// If shared read locks are abused during namespace changes, concurrent execution
			// triggers directory entries errors or parental inode corruption.
			if _, err := c.fs.Hash(fpath); err != nil {
				errChan <- err
			}
		}(files[i])
	}

	wg.Wait()
	close(errChan)

	if len(errChan) > 0 {
		firstErr := <-errChan
		return fmt.Errorf("namespace race condition captured: %w", firstErr)
	}

	return nil
}

// ProcPidMapsIsolationTest ensures memory maps created via mmap do not leak
// backend/lower storage infrastructure path trails into /proc/self/maps.
// It uses a hard link layout to force separate namespace validation passes.
func (c *casTest) ProcPidMapsIsolationTest(basePath, linkPath string) error {
	pageSize := os.Getpagesize()

	// 1. Create the primary target file and establish an initial size footprint
	f1, err := os.OpenFile(basePath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return fmt.Errorf("failed to initialize basePath: %w", err)
	}
	defer f1.Close()

	initialPayload := []byte(strings.Repeat("M", pageSize))
	if _, err := f1.Write(initialPayload); err != nil {
		return fmt.Errorf("failed to populate payload: %w", err)
	}

	// 2. Form an independent hard link view pointing to the same layout entry node
	if err := os.Link(basePath, linkPath); err != nil {
		return fmt.Errorf("failed to instantiate structural hard link: %w", err)
	}
	defer os.Remove(linkPath)

	f2, err := os.OpenFile(linkPath, os.O_RDONLY, 0)
	if err != nil {
		return fmt.Errorf("failed to open hard link descriptor: %w", err)
	}
	defer f2.Close()

	// 3. Instantiate parallel memory mappings against BOTH unique path references
	mmap1, err := unix.Mmap(int(f1.Fd()), 0, pageSize, unix.PROT_READ, unix.MAP_SHARED)
	if err != nil {
		return fmt.Errorf("mmap failed on basePath: %w", err)
	}
	defer unix.Munmap(mmap1)

	mmap2, err := unix.Mmap(int(f2.Fd()), 0, pageSize, unix.PROT_READ, unix.MAP_SHARED)
	if err != nil {
		return fmt.Errorf("mmap failed on linkPath: %w", err)
	}
	defer unix.Munmap(mmap2)

	// Dirty-read a single byte to ensure the architecture instantiates the page tables
	_ = mmap1[0]
	_ = mmap2[0]

	// 4. Open and scan /proc/self/maps to inspect our active address registrations
	mapsFile, err := os.Open("/proc/self/maps")
	if err != nil {
		return fmt.Errorf("failed to access active process maps context: %w", err)
	}
	defer mapsFile.Close()

	var matchedBase, matchedLink bool
	scanner := bufio.NewScanner(mapsFile)

	for scanner.Scan() {
		line := scanner.Text()

		// LEAK DETECTOR 1: Search for unauthorized scratch space markers
		if strings.Contains(line, "cas_scratch") || strings.Contains(line, ".blobs") {
			return fmt.Errorf("CRITICAL SECURITY LEAK: Backend path trace leaked to /proc/maps: %s", line)
		}

		// Track that our virtual maps are resolving to the correct user-space paths
		if strings.Contains(line, basePath) {
			matchedBase = true
		}
		if strings.Contains(line, linkPath) {
			matchedLink = true
		}
	}

	if err := scanner.Err(); err != nil {
		return fmt.Errorf("error reading maps buffer stream: %w", err)
	}

	// 5. ENFORCE SEPARATE VIEWS ASSEMBLED BY THE VFS LAYER
	if !matchedBase {
		return fmt.Errorf("isolation failure: Expected to find user path %q in maps table", basePath)
	}
	if !matchedLink {
		return fmt.Errorf("isolation failure: Expected to find separate hard link target view %q in maps table", linkPath)
	}

	return nil
}

// SpliceStreamingMetadataTest forces the OS to transfer data using the kernel's
// splice/pipe subsystem, verifying that .splice_write updates tracking boundaries correctly.
func (c *casTest) SpliceStreamingMetadataTest(fpath string) error {
	// Sizes mirroring your dmesg anomalies
	targetSizes := []int{14971, 19913, 21242}

	for i, size := range targetSizes {
		// Clean out the old file container
		_ = os.Remove(fpath)

		f, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
		if err != nil {
			return fmt.Errorf("[Iteration %d] open target failed: %w", i, err)
		}

		// 1. Instantiate a low-level Unix pipe descriptor pair [Reader, Writer]
		pipeFDs := []int{0, 0}
		if err := syscall.Pipe(pipeFDs); err != nil {
			f.Close()
			return fmt.Errorf("failed to allocate kernel pipeline: %w", err)
		}
		pipeRead := os.NewFile(uintptr(pipeFDs[0]), "pipe_reader")
		pipeWrite := os.NewFile(uintptr(pipeFDs[1]), "pipe_writer")

		// 2. Populate the pipeline from userspace
		payload := []byte(strings.Repeat("S", size))

		// Run the write-side transfer concurrently to avoid pipe block deadlocks
		// if the payload buffer exceeds standard kernel pipe capacities (~64KB)
		go func() {
			defer pipeWrite.Close()
			_, _ = pipeWrite.Write(payload)
		}()

		// 3. Invoke raw unix.Splice to pipe data straight from the pipe to our file descriptor
		// This bypasses write() completely and triggers the driver's .splice_write handler.
		var offset int64 = 0
		splSize, err := unix.Splice(int(pipeRead.Fd()), nil, int(f.Fd()), &offset, size, unix.SPLICE_F_MOVE)

		pipeRead.Close()
		f.Close()

		if err != nil {
			return fmt.Errorf("[Iteration %d] kernel splice transfer failed: %w", i, err)
		}
		if int(splSize) != size {
			return fmt.Errorf("[Iteration %d] short splice transfer: expected %d, got %d", i, size, splSize)
		}

		// 4. CRITICAL CHECK: Immediately query the Snapshot engine via HashSize.
		// If .splice_write updated the lower file but left the upper inode's i_size
		// floating or mismatched, this evaluation will trigger a size mismatch.
		_, actualSize, err := c.fs.HashSize(fpath)
		if err != nil {
			return fmt.Errorf("[Iteration %d] post-splice snapshot pass failed: %w", i, err)
		}

		expectedSize := int64(size)
		if actualSize != expectedSize {
			return fmt.Errorf("[Iteration %d] SPLICE METADATA DRIFT! Userspace spliced %d bytes, but snapshot engine expected %d",
				i, expectedSize, actualSize)
		}
	}

	return nil
}
func (c *casTest) RenameAtomicMetadataSwapTest(baseDir string) error {
	// Replicate the exact sizes and transitions from your dmesg log blocks
	sizeOld := int64(14971)
	sizeNew := int64(19913)

	productionPath := filepath.Join(baseDir, "production.config")
	scratchPath := filepath.Join(baseDir, "scratch.tmp")

	// 1. Setup the old base file asset
	fOld, err := os.OpenFile(productionPath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return err
	}
	_ = fOld.Truncate(sizeOld)
	fOld.Close()

	// Populate the metadata cache layer for the original file path context
	_, _, _ = c.fs.HashSize(productionPath)

	// 2. Setup the incoming replacement file asset with the target size delta
	fNew, err := os.OpenFile(scratchPath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return err
	}
	_ = fNew.Truncate(sizeNew)
	fNew.Close()

	// 3. ATOMIC SWAP: Rename the scratch target directly over the production track
	// This triggers your driver's rename entry point, switching underlying dentries.
	if err := os.Rename(scratchPath, productionPath); err != nil {
		return fmt.Errorf("atomic rename operation failed: %w", err)
	}

	// 4. CRITICAL CHECK: Query the snapshot size metrics on the updated path target
	_, actualSize, err := c.fs.HashSize(productionPath)
	if err != nil {
		return fmt.Errorf("post-rename snapshot query failed: %w", err)
	}

	if actualSize != sizeNew {
		return fmt.Errorf("RENAME METADATA DRIFT DETECTED! Expected size %d, but snapshot system read %d. Old cached size leaked!",
			sizeNew, actualSize)
	}

	return nil
}

// ActiveWriteStreamInterleaveTest simulates a build tool holding a file handle open
// and continuously appending data, while a parallel thread requests snapshots.
// It verifies that the driver handles concurrent standard writes cleanly without returning EBUSY.
func (c *casTest) ActiveWriteStreamInterleaveTest(fpath string) error {
	_ = os.Remove(fpath)

	// 1. Open the file handle and hold it open for writing
	f, err := os.OpenFile(fpath, os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0644)
	if err != nil {
		return fmt.Errorf("failed to open write handle: %w", err)
	}
	defer f.Close()

	var stop int32 = 0
	var wg sync.WaitGroup
	wg.Add(2)

	// Thread A: Stream Appender (Grows file size non-stop)
	go func() {
		defer wg.Done()
		chunk := []byte("X")
		for atomic.LoadInt32(&stop) == 0 {
			_, _ = f.Write(chunk)
			time.Sleep(50 * time.Microsecond)
		}
	}()

	// Thread B: Snapshot Poller (The Auditor)
	var testErr error
	go func() {
		defer wg.Done()
		time.Sleep(5 * time.Millisecond)

		// 2. Query HashSize while the write channel is actively pinned open.
		// Since we handle page cache flushing under locks transparently now,
		// this call should execute COMPLETELY CLEANLY with no errors.
		_, _, err := c.fs.HashSize(fpath)
		if err != nil {
			testErr = fmt.Errorf("snapshot failed during active non-blocking write stream: %w", err)
		}

		atomic.StoreInt32(&stop, 1)
	}()

	wg.Wait()
	return testErr
}

func (c *casTest) DynamicPromotionAppendTest(fpath string) error {
	pageSize := os.Getpagesize()

	// 1. Pre-populate file with clean data exactly matching architecture page sizing
	fInit, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return fmt.Errorf("failed to create base file: %w", err)
	}
	// Write "HEAD" padded out to exactly one page size
	headPayload := append([]byte("HEAD"), make([]byte, pageSize-4)...)
	if _, err := fInit.Write(headPayload); err != nil {
		fInit.Close()
		return err
	}
	fInit.Close()

	// 2. Snapshot the file to force it back into an immutable CAS persistent provider state (CAS_CACHE_UNLOADED)
	_, _, err = c.fs.HashSize(fpath)
	if err != nil {
		return fmt.Errorf("failed to snapshot initialization baseline: %w", err)
	}

	// 3. Open with explicit O_RDONLY to cache a strict read-only backend descriptor proxy
	fRo, err := os.OpenFile(fpath, os.O_RDONLY, 0)
	if err != nil {
		return fmt.Errorf("failed to open read-only descriptor: %w", err)
	}
	defer fRo.Close()

	// Trigger a read pass to force the state machine to settle into CAS_CACHE_READ_ACTIVE
	readBuf := make([]byte, 4)
	if _, err := fRo.ReadAt(readBuf, 0); err != nil {
		return fmt.Errorf("initial read-only proxy pinning failed: %w", err)
	}

	// 4. Open a completely separate parallel file handle with O_APPEND write intent.
	// Because it targets the exact same underlying cas_node, it will attempt to re-use
	// or upgrade the existing read-only layout descriptor context during write checks.
	fAppend, err := os.OpenFile(fpath, os.O_WRONLY|os.O_APPEND, 0644)
	if err != nil {
		return fmt.Errorf("failed to open append writer handle: %w", err)
	}
	defer fAppend.Close()

	// 5. Issue the write transaction. This forces cas_file_ensure_write to notice
	// that the cached backing file lacks FMODE_WRITE, breaking out of the shortcut
	// and triggering a real promotion pass in cas_ensure_real_file.
	if _, err := fAppend.Write([]byte("TAIL")); err != nil {
		return fmt.Errorf("promoted write call failed: %w", err)
	}
	fAppend.Close()
	fRo.Close()

	// 6. Read back text structure to verify target outcome layout
	content, err := os.ReadFile(fpath)
	if err != nil {
		return fmt.Errorf("failed to read back validation file: %w", err)
	}

	// On an unpatched kernel, because the upgraded backing file drops O_APPEND,
	// the write offset will fallback to 0, overwriting the "HEAD" prefix.
	if string(content[:4]) != "HEAD" {
		return fmt.Errorf("CRITICAL BUG: O_APPEND flag lost during structural promotion! Overwrote head block.")
	}

	// Check if the tail appended correctly at the original file end boundary (index = pageSize)
	tailOffset := int64(pageSize)
	if string(content[tailOffset:tailOffset+4]) != "TAIL" {
		return fmt.Errorf("O_APPEND data missing from file tail boundary. Content length: %d", len(content))
	}

	return nil
}

// SharedInodeMultipleFdReadWritePromotionTest verifies that when two separate FDs
// point to the same underlying inode, modifying via FD2 correctly promotions the
// underlying storage without causing offset anomalies or stale data access on FD1.
func (c *casTest) SharedInodeMultipleFdReadWritePromotionTest(fpath string) error {
	// 1. Establish initial file state
	fInit, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return fmt.Errorf("failed to create base file: %w", err)
	}
	if _, err := fInit.Write([]byte("ORIGINAL_DATA_CONTENT")); err != nil {
		fInit.Close()
		return err
	}
	fInit.Close()

	// Push file to CAS_CACHE_UNLOADED / stable storage baseline
	if _, _, err = c.fs.HashSize(fpath); err != nil {
		return fmt.Errorf("failed baseline snapshot: %w", err)
	}

	// 2. Open FD1 (Read-Only) to move state machine into CAS_CACHE_READ_ACTIVE
	fd1, err := os.OpenFile(fpath, os.O_RDONLY, 0)
	if err != nil {
		return fmt.Errorf("failed to open fd1: %w", err)
	}
	defer fd1.Close()

	buf1 := make([]byte, 8)
	if _, err := fd1.ReadAt(buf1, 0); err != nil {
		return fmt.Errorf("fd1 initial read failed: %w", err)
	}
	if string(buf1) != "ORIGINAL" {
		return fmt.Errorf("unexpected content on fd1: %s", string(buf1))
	}

	// 3. Open FD2 (Read-Write) targeting the exact same path
	fd2, err := os.OpenFile(fpath, os.O_RDWR, 0)
	if err != nil {
		return fmt.Errorf("failed to open fd2: %w", err)
	}
	defer fd2.Close()

	// 4. Write through FD2, triggering mid-flight write promotion inside the kernel via cas_blob_ensure_write
	if _, err := fd2.WriteAt([]byte("MODIFIED"), 0); err != nil {
		return fmt.Errorf("fd2 write promotion failed: %w", err)
	}

	// 5. CRITICAL CHECK: Verify FD1 immediately reflects changes through its proxy handle mapping
	buf1Updated := make([]byte, 8)
	if _, err := fd1.ReadAt(buf1Updated, 0); err != nil {
		return fmt.Errorf("fd1 read post-promotion failed: %w", err)
	}
	if string(buf1Updated) != "MODIFIED" {
		return fmt.Errorf("VFS proxy desync! FD1 did not track the storage promotion layer. Got: %s", string(buf1Updated))
	}

	return nil
}

// MmapInterleavePageFaultRaceTest ensures that mapped memory regions gracefully handle
// an in-flight storage proxy swap caused by a write() syscall executed from a separate thread.
func (c *casTest) MmapInterleavePageFaultRaceTest(fpath string) error {
	pageSize := os.Getpagesize()

	f, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return fmt.Errorf("failed to initialize file: %w", err)
	}
	defer f.Close()

	// Fill file with continuous baseline pattern
	pattern := bytes.Repeat([]byte("R"), pageSize)
	if _, err := f.Write(pattern); err != nil {
		return err
	}

	// Move into CAS_CACHE_READ_ACTIVE state
	if _, _, err = c.fs.HashSize(fpath); err != nil {
		return err
	}

	// Reopen to map
	mFile, err := os.OpenFile(fpath, os.O_RDONLY, 0)
	if err != nil {
		return err
	}
	defer mFile.Close()

	// Set up read-only mmap tracking the proxy mapping
	mmapData, err := unix.Mmap(int(mFile.Fd()), 0, pageSize, unix.PROT_READ, unix.MAP_SHARED)
	if err != nil {
		return fmt.Errorf("mmap failure: %w", err)
	}
	defer unix.Munmap(mmapData)

	var wg sync.WaitGroup
	var faultErr error
	var stopFlag int32 = 0

	wg.Add(2)

	// Thread A: Constantly hit the memory mapping to trigger page faults / translation reads
	go func() {
		defer wg.Done()
		for atomic.LoadInt32(&stopFlag) == 0 {
			val := mmapData[pageSize-1]
			if val != 'R' && val != 'W' {
				faultErr = fmt.Errorf("memory mapping payload corrupted during transition: %v", val)
				return
			}
			time.Sleep(10 * time.Microsecond)
		}
	}()

	// Thread B: Force mid-flight write execution to prompt an upgrade swap inside casfs
	go func() {
		defer wg.Done()
		time.Sleep(2 * time.Millisecond)

		wFile, err := os.OpenFile(fpath, os.O_WRONLY, 0)
		if err != nil {
			return
		}
		defer wFile.Close()

		// Write payload forcing state machine shift to CAS_CACHE_WRITE_ACTIVE
		_, _ = wFile.WriteAt(bytes.Repeat([]byte("W"), pageSize), 0)
		atomic.StoreInt32(&stopFlag, 1)
	}()

	wg.Wait()
	return faultErr
}

// SuddenEvictionDuringReadActiveTest verifies that clean backend file proxy tracking
// references do not trigger use-after-free bugs when dropped back down to UNLOADED
// while slow parallel stream loops are reading blocks.
func (c *casTest) SuddenEvictionDuringReadActiveTest(fpath string) error {
	_ = os.Remove(fpath)

	f, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return err
	}
	if _, err := f.Write(bytes.Repeat([]byte("K"), 4096)); err != nil {
		f.Close()
		return err
	}
	f.Close()

	// Push file to CAS_CACHE_READ_ACTIVE
	fReader, err := os.OpenFile(fpath, os.O_RDONLY, 0)
	if err != nil {
		return err
	}
	defer fReader.Close()

	initialBuf := make([]byte, 10)
	if _, err := fReader.Read(initialBuf); err != nil {
		return err
	}

	var wg sync.WaitGroup
	var asyncErr error
	wg.Add(2)

	// Thread A: Perform a slow, fragmented sequential read access pass
	go func() {
		defer wg.Done()
		time.Sleep(1 * time.Millisecond)
		slowBuf := make([]byte, 100)
		// This must complete cleanly without hitting a broken or unmapped kernel tracking structure
		if _, err := fReader.ReadAt(slowBuf, 500); err != nil && err != io.EOF {
			asyncErr = fmt.Errorf("read broke during concurrent cache eviction pass: %w", err)
		}
	}()

	// Thread B: Force an explicit global metadata sync / cache drop layout on the path,
	// asserting that active reference counts keep the local descriptor safe.
	go func() {
		defer wg.Done()
		time.Sleep(2 * time.Millisecond)
		// Requesting hash metric calculation pushes structural synchronization down the VFS pipeline
		_, _, _ = c.fs.HashSize(fpath)
	}()

	wg.Wait()
	return asyncErr
}

// ConcurrentReadWriteIterStressTest targets the synchronized performance bounds of our iterators
// by spawning tightly-wound thread groups interleaving standard write sweeps with read passes.
func (c *casTest) ConcurrentReadWriteIterStressTest(fpath string) error {
	_ = os.Remove(fpath)

	f, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return err
	}
	defer f.Close()

	if _, err := f.Write(bytes.Repeat([]byte("X"), 2048)); err != nil {
		return err
	}

	var wg sync.WaitGroup
	var threadErr error
	const threadCount = 4
	const loopIterations = 100

	// Launch explicitly bounded readers and writers to eliminate spin deadlocks
	for i := 0; i < threadCount; i++ {
		// Reader Thread Block
		wg.Add(1)
		go func() {
			defer wg.Done()
			buf := make([]byte, 256)
			for j := 0; j < loopIterations; j++ {
				_, err := f.ReadAt(buf, 0)
				if err != nil && err != io.EOF {
					threadErr = err
					return
				}
			}
		}()

		// Writer Thread Block
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			update := bytes.Repeat([]byte{byte('0' + id)}, 256)
			for j := 0; j < loopIterations; j++ {
				_, err := f.WriteAt(update, 0)
				if err != nil {
					threadErr = err
					return
				}
			}
		}(i)
	}

	wg.Wait()
	return threadErr
}

func (c *casTest) ActiveOpenCloseValidationTest(fpath string) error {
	// 1. Create and populate a standard target file to establish storage contents
	fInit, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return fmt.Errorf("failed to create target file: %w", err)
	}
	if _, err := fInit.Write([]byte("unmount_test_payload_string")); err != nil {
		fInit.Close()
		return err
	}
	fInit.Close()

	fActive, err := os.OpenFile(fpath, os.O_RDONLY, 0)
	if err != nil {
		return fmt.Errorf("failed to open active read descriptor: %w", err)
	}

	buf := make([]byte, 7)
	if _, err := fActive.Read(buf); err != nil {
		fActive.Close()
		return fmt.Errorf("failed active read transaction: %w", err)
	}

	fActive.Close()

	return nil
}

// ConcurrentPromotionTruncateStressTest forces parallel read-only pinning loops
// to violently collide with active write promotions and truncation loops to isolate proxy leaks.
func (c *casTest) ConcurrentPromotionTruncateStressTest(fpath string) error {
	var wg sync.WaitGroup
	var threadErr error
	workers := 10
	iterations := 500

	// Pre-populate target asset
	fInit, _ := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	fInit.Write([]byte(strings.Repeat("A", 2048)))
	fInit.Close()
	_, _ = c.fs.Hash(fpath) // Push to stable CAS tier

	// Spawn concurrent readers that force CAS_CACHE_READ_ACTIVE
	for i := 0; i < workers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < iterations; j++ {
				f, err := os.OpenFile(fpath, os.O_RDONLY, 0)
				if err != nil {
					return
				}
				buf := make([]byte, 10)
				f.ReadAt(buf, 0)
				f.Close() // Triggers cas_release
			}
		}()
	}

	// Spawn a concurrent writer that violently truncates and mutates layout state
	wg.Add(1)
	go func() {
		defer wg.Done()
		for j := 0; j < iterations; j++ {
			f, err := os.OpenFile(fpath, os.O_RDWR, 0)
			if err != nil {
				return
			}
			f.Truncate(int64(j % 1024))
			f.WriteAt([]byte("B"), 0)
			f.Close()
			time.Sleep(10 * time.Microsecond)
		}
	}()

	wg.Wait()
	return threadErr
}

// DirectBinaryExecutionUnmountTest isolates whether running a binary compiled on
// the mount point leaks text segment execution references inside the VFS layer.
func (c *casTest) DirectBinaryExecutionUnmountTest(fpath string) error {
	// 1. Write out a tiny, valid C source code file
	cSrc := fpath + "_test.c"
	f, err := os.OpenFile(cSrc, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0755)
	if err != nil {
		return fmt.Errorf("failed to create C file: %w", err)
	}
	_, _ = f.Write([]byte("#include <stdio.h>\nint main() { printf(\"ping\\n\"); return 0; }\n"))
	f.Close()

	// 2. Compile it to a local binary file on the mount point
	binaryPath := fpath + "_test.bin"
	cmdCompile := exec.Command("gcc", cSrc, "-o", binaryPath)
	if err := cmdCompile.Run(); err != nil {
		return fmt.Errorf("failed to compile mock test binary: %w", err)
	}

	// 3. Execute the binary file directly from our filesystem
	cmdExec := exec.Command(binaryPath)
	output, err := cmdExec.Output()
	if err != nil {
		return fmt.Errorf("failed to run mock binary: %w", err)
	}
	if !strings.Contains(string(output), "ping") {
		return fmt.Errorf("unexpected binary execution trace output: %q", string(output))
	}

	// Clean up user-space files
	os.Remove(cSrc)
	os.Remove(binaryPath)

	// Return control. Your test runner framework will now attempt to unmount.
	// If binary text segment pinning leaks, the runner unmount phase will fail.
	return nil
}

func (c *casTest) ARISizeTest(fpath, ari string, size int64, expectedPayload []byte) error {
	// 1. Create target empty file
	f, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return fmt.Errorf("failed to create ARI test file: %w", err)
	}
	f.Close()

	// 2. Set the ARI and size on the target file node
	if err := c.fs.SetARISize(fpath, ari, size); err != nil {
		return fmt.Errorf("SetARISize failed: %w", err)
	}

	// 3. Verify reading back the ARI and size
	readAri, readSize, err := c.fs.ARISize(fpath)
	if err != nil {
		return fmt.Errorf("ARISize read failed: %w", err)
	}
	if readAri != ari {
		return fmt.Errorf("ARI mismatch: expected %q, got %q", ari, readAri)
	}
	if readSize != size {
		return fmt.Errorf("Size mismatch: expected %d, got %d", size, readSize)
	}

	// 4. Verify reading contents through VFS resolves against the ARI backing store
	data, err := os.ReadFile(fpath)
	if err != nil {
		return fmt.Errorf("failed to read content back from ARI file: %w", err)
	}
	if !bytes.Equal(data, expectedPayload) {
		return fmt.Errorf("ARI content mismatch: expected %q, got %q", string(expectedPayload), string(data))
	}

	return nil
}
func (c *casTest) ARIWriteAndSnapshotTest(fpath, ari string, origPayload, newPayload []byte) error {
	// 1. Create target file and set up initial ARI binding
	f, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return fmt.Errorf("failed to create file: %w", err)
	}
	f.Close()

	if err := c.fs.SetARISize(fpath, ari, int64(len(origPayload))); err != nil {
		return fmt.Errorf("SetARISize failed: %w", err)
	}
	// 2. Perform a write to the ARI-backed file (overwriting/modifying it)
	fWrite, err := os.OpenFile(fpath, os.O_WRONLY|os.O_TRUNC, 0644)
	if err != nil {
		return fmt.Errorf("failed to open ARI file for write: %w", err)
	}
	if _, err := fWrite.Write(newPayload); err != nil {
		fWrite.Close()
		return fmt.Errorf("failed to write new payload: %w", err)
	}
	fWrite.Close()
	// 3. Verify reading back returns the new modified content
	readData, err := os.ReadFile(fpath)
	if err != nil {
		return fmt.Errorf("failed to read back modified content: %w", err)
	}
	if !bytes.Equal(readData, newPayload) {
		return fmt.Errorf("content mismatch: expected %q, got %q", string(newPayload), string(readData))
	}
	// 4. Verify querying ARISize now fails or reflects transition away from cas_type_ari
	if _, _, err := c.fs.ARISize(fpath); err == nil {
		return fmt.Errorf("expected ARISize to fail on dirty/modified file, but it succeeded")
	}
	// 5. Snapshot the file via Hash and verify it returns a valid non-empty hash
	h, err := c.fs.Hash(fpath)
	if err != nil {
		return fmt.Errorf("Hash snapshot failed post-write: %w", err)
	}
	if h.IsEmptyFile() || h.IsZero() {
		return fmt.Errorf("expected valid hash post-write snapshot, got empty/zero: %s", h)
	}
	return nil
}

// ARISetOnHashedFileTest verifies that setting an ARI on a file with a valid hash
func (c *casTest) ARISetOnHashedFileTest(fpath, content, ari string) error {
	if err := FileTestContent(fpath, content); err != nil {
		return err
	}

	// Calculate and set a valid content hash to move the file out of an empty state
	h, sz, err := c.fs.HashSize(fpath)
	if err != nil {
		return fmt.Errorf("failed to get HashSize for baseline file: %w", err)
	}
	if h.IsEmptyFile() {
		return fmt.Errorf("expected non-empty hash for populated file")
	}

	if err := c.fs.SetARISize(fpath, ari, sz); err != nil {
		return fmt.Errorf("failed to SetARISize on a hashed file")
	}

	return nil
}

// ARISetSameARITest verifies that setting an ARI-backed file to the exact same ARI succeeds.
func (c *casTest) ARISetSameARITest(fpath, ari string, size int64) error {
	f, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return fmt.Errorf("failed to create test file: %w", err)
	}
	f.Close()

	if err := c.fs.SetARISize(fpath, ari, size); err != nil {
		return fmt.Errorf("initial SetARISize failed: %w", err)
	}

	// Re-applying the exact same ARI should succeed cleanly
	if err := c.fs.SetARISize(fpath, ari, size); err != nil {
		return fmt.Errorf("re-setting the same ARI failed: %w", err)
	}

	readAri, readSize, err := c.fs.ARISize(fpath)
	if err != nil {
		return fmt.Errorf("ARISize read back failed: %w", err)
	}
	if readAri != ari || readSize != size {
		return fmt.Errorf("readback mismatch after re-setting same ARI: got %s:%d, expected %s:%d", readAri, readSize, ari, size)
	}

	return nil
}

// ARISetDifferentARITest verifies setting an ARI-backed file to a different ARI
func (c *casTest) ARISetDifferentARITest(fpath, ari1, ari2 string, size int64) error {
	f, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return fmt.Errorf("failed to create test file: %w", err)
	}
	f.Close()

	if err := c.fs.SetARISize(fpath, ari1, size); err != nil {
		return fmt.Errorf("initial SetARISize failed: %w", err)
	}

	if err := c.fs.SetARISize(fpath, ari2, size); err != nil {
		return fmt.Errorf("failed to SetARISize with a different ARI")
	}

	return nil
}

// ARISetHashOnARIFileTest verify calling SetHashSize on an ARI-backed file
func (c *casTest) ARISetHashOnARIFileTest(fpath, ari string, size int64) error {
	f, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return fmt.Errorf("failed to create test file: %w", err)
	}
	f.Close()

	if err := c.fs.SetARISize(fpath, ari, size); err != nil {
		return fmt.Errorf("initial SetARISize failed: %w", err)
	}

	dummyHash := c.hasher.EmptyFileHash()
	if err := c.fs.SetHashSize(fpath, dummyHash, size); err != nil {
		return fmt.Errorf("SetHashSize failed on an ARI-backed")
	}

	return nil
}

// ARISetInvalidLengthTest verifies that setting an empty or oversized ARI fails.
func (c *casTest) ARISetInvalidLengthTest(fpath string) error {
	f, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return fmt.Errorf("failed to create test file: %w", err)
	}
	f.Close()

	// Setting empty ARI should fail
	if err := c.fs.SetARISize(fpath, "", 1024); err == nil {
		return fmt.Errorf("expected SetARISize with empty ARI to fail, but it succeeded")
	}

	// Setting ARI exceeding 4096 bytes should fail
	oversizedAri := strings.Repeat("a", 4097)
	if err := c.fs.SetARISize(fpath, oversizedAri, 1024); err == nil {
		return fmt.Errorf("expected SetARISize with oversized ARI to fail, but it succeeded")
	}

	return nil
}

// BackendStatARITest verifies that BackendStat accurately returns IsARI = true,
// expected ARI identifier, file size, and mode on clean ARI-backed files.
func (c *casTest) BackendStatARITest(fpath, ari string, size int64, payload []byte, expectedMode fs.FileMode) error {
	f, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, expectedMode)
	if err != nil {
		return fmt.Errorf("failed to create ARI test file: %w", err)
	}
	f.Close()

	if err := c.fs.SetARISize(fpath, ari, size); err != nil {
		return fmt.Errorf("SetARISize failed: %w", err)
	}

	stat, err := c.fs.BackendStat(fpath)
	if err != nil {
		return fmt.Errorf("BackendStat failed on ARI file: %w", err)
	}

	if !stat.IsARI {
		return fmt.Errorf("expected IsARI to be true, got false")
	}
	if stat.ARI != ari {
		return fmt.Errorf("ARI mismatch: expected %q, got %q", ari, stat.ARI)
	}
	if stat.Size != size {
		return fmt.Errorf("Size mismatch: expected %d, got %d", size, stat.Size)
	}
	if stat.Mode != expectedMode {
		return fmt.Errorf("Mode mismatch: expected %v, got %v", expectedMode, stat.Mode)
	}

	return nil
}

// BackendStatHashedFileTest verifies that BackendStat accurately returns IsARI = false,
// a valid content Hash, file size, and correct file mode on standard hashed files.
func (c *casTest) BackendStatHashedFileTest(fpath, content string, expectedMode fs.FileMode) error {
	if err := FileTestContent(fpath, content); err != nil {
		return err
	}

	// Set desired file mode explicitly if needed
	if err := os.Chmod(fpath, expectedMode); err != nil {
		return fmt.Errorf("failed to chmod test file: %w", err)
	}

	// Calculate and snapshot hash
	expectedHash, expectedSize, err := c.fs.HashSize(fpath)
	if err != nil {
		return fmt.Errorf("HashSize failed: %w", err)
	}

	stat, err := c.fs.BackendStat(fpath)
	if err != nil {
		return fmt.Errorf("BackendStat failed on hashed file: %w", err)
	}

	if stat.IsARI {
		return fmt.Errorf("expected IsARI to be false on standard hashed file, got true")
	}
	if !stat.Hash.Equals(expectedHash) {
		return fmt.Errorf("Hash mismatch: expected %s, got %s", expectedHash, stat.Hash)
	}
	if stat.Size != expectedSize {
		return fmt.Errorf("Size mismatch: expected %d, got %d", expectedSize, stat.Size)
	}
	if stat.Mode != expectedMode {
		return fmt.Errorf("Mode mismatch: expected %v, got %v", expectedMode, stat.Mode)
	}

	return nil
}

// BackendStatDirtyARIFileTest verifies mode preservation alongside dirty snapshotting.
func (c *casTest) BackendStatDirtyARIFileTest(fpath, ari string, origPayload, newPayload []byte, expectedMode fs.FileMode) error {
	f, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, expectedMode)
	if err != nil {
		return fmt.Errorf("failed to create ARI test file: %w", err)
	}
	f.Close()

	if err := c.fs.SetARISize(fpath, ari, int64(len(origPayload))); err != nil {
		return fmt.Errorf("SetARISize failed: %w", err)
	}

	// Modify the ARI file to make it dirty
	fWrite, err := os.OpenFile(fpath, os.O_WRONLY|os.O_TRUNC, expectedMode)
	if err != nil {
		return fmt.Errorf("failed to open ARI file for writing: %w", err)
	}
	if _, err := fWrite.Write(newPayload); err != nil {
		fWrite.Close()
		return fmt.Errorf("failed to write new payload: %w", err)
	}
	fWrite.Close()

	stat, err := c.fs.BackendStat(fpath)
	if err != nil {
		return fmt.Errorf("BackendStat failed on dirty ARI file: %w", err)
	}

	if stat.IsARI {
		return fmt.Errorf("expected dirty file to transition away from ARI, but BackendStat returned IsARI = true")
	}
	if stat.Hash.IsEmptyFile() || stat.Hash.IsZero() {
		return fmt.Errorf("expected valid non-empty content hash after snapshot, got: %s", stat.Hash)
	}
	if stat.Size != int64(len(newPayload)) {
		return fmt.Errorf("expected updated size %d, got %d", len(newPayload), stat.Size)
	}
	if stat.Mode != expectedMode {
		return fmt.Errorf("Mode mismatch: expected %v, got %v", expectedMode, stat.Mode)
	}

	return nil
}
func (c *casTest) ARIMissingProviderDataTest(fpath, ari string, size int64) error {
	f, err := os.OpenFile(fpath, os.O_RDWR|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return fmt.Errorf("failed to create ARI test file: %w", err)
	}
	f.Close()

	if err := c.fs.SetARISize(fpath, ari, size); err != nil {
		return fmt.Errorf("SetARISize failed: %w", err)
	}

	fOpen, err := os.OpenFile(fpath, os.O_RDONLY, 0)
	if err == nil {
		fOpen.Close()
		return fmt.Errorf("vulnerability: open succeeded on unbacked ARI file")
	}

	var pathErr *os.PathError
	if errors.As(err, &pathErr) {
		if !errors.Is(pathErr.Err, syscall.ENOENT) {
			return fmt.Errorf("unexpected error on open: expected ENOENT, got: %v", pathErr.Err)
		}
	} else if !errors.Is(err, syscall.ENOENT) {
		return fmt.Errorf("unexpected error on open: expected ENOENT, got: %v", err)
	}

	return nil
}

// CopyFileRangeCleanDestinationTest verifies that executing copy_file_range(2)
// targeting an existing clean/hashed destination node duplicates the backing blob
// cleanly into scratch space without triggering recursive VFS splice panics or WARNs.
func (c *casTest) CopyFileRangeCleanDestinationTest(srcPath, dstPath string) error {
	const srcSize = 64 * 1024          // 64 KB
	const dstInitialSize = 1024 * 1024 // 1 MB (ensures len > 0 when duplicating backing store)
	const copyLen = 32 * 1024          // 32 KB

	srcPayload := bytes.Repeat([]byte("S"), srcSize)
	if err := os.WriteFile(srcPath, srcPayload, 0644); err != nil {
		return fmt.Errorf("failed to write source file: %w", err)
	}

	dstPayload := bytes.Repeat([]byte("D"), dstInitialSize)
	if err := os.WriteFile(dstPath, dstPayload, 0644); err != nil {
		return fmt.Errorf("failed to write initial destination file: %w", err)
	}

	dstHash, sz, err := c.fs.HashSize(dstPath)
	if err != nil {
		return fmt.Errorf("failed to snapshot destination file: %w", err)
	}
	if dstHash.IsEmptyFile() || dstHash.IsZero() {
		return fmt.Errorf("expected valid non-empty hash for destination file, got %s", dstHash)
	}
	if sz != dstInitialSize {
		return fmt.Errorf("expected destination size %d, got %d", dstInitialSize, sz)
	}

	dirty, err := c.fs.Dirty(dstPath)
	if err != nil {
		return fmt.Errorf("failed to query dirty state: %w", err)
	}
	if dirty {
		return fmt.Errorf("destination file is still dirty after HashSize")
	}

	fSrc, err := os.OpenFile(srcPath, os.O_RDONLY, 0)
	if err != nil {
		return fmt.Errorf("failed to open source for copy_file_range: %w", err)
	}
	defer fSrc.Close()

	fDst, err := os.OpenFile(dstPath, os.O_WRONLY, 0644)
	if err != nil {
		return fmt.Errorf("failed to open destination for copy_file_range: %w", err)
	}
	defer fDst.Close()

	var offIn int64 = 0
	var offOut int64 = 0
	copied, err := unix.CopyFileRange(int(fSrc.Fd()), &offIn, int(fDst.Fd()), &offOut, copyLen, 0)
	if err != nil {
		return fmt.Errorf("unix.CopyFileRange failed: %w", err)
	}
	if copied != copyLen {
		return fmt.Errorf("short copy_file_range: expected %d bytes, copied %d", copyLen, copied)
	}

	_ = fSrc.Close()
	_ = fDst.Close()

	readBack, err := os.ReadFile(dstPath)
	if err != nil {
		return fmt.Errorf("failed to read back destination file: %w", err)
	}
	if !bytes.Equal(readBack[:copyLen], srcPayload[:copyLen]) {
		return fmt.Errorf("content mismatch on copied region")
	}
	if !bytes.Equal(readBack[copyLen:], dstPayload[copyLen:]) {
		return fmt.Errorf("unwritten tail region of destination was corrupted")
	}

	return nil
}

// XattrHashUpdateTimestampsTest verifies SetHashSize advances mtime and ctime.
func (c *casTest) XattrHashUpdateTimestampsTest(fpath string) error {
	if err := os.WriteFile(fpath, []byte("baseline data"), 0644); err != nil {
		return err
	}

	t0, err := getInodeTimes(fpath)
	if err != nil {
		return err
	}
	_, _, err = c.fs.HashSize(fpath)
	if err != nil {
		return err
	}
	time.Sleep(15 * time.Millisecond)

	// SetHashSize triggers our internal cas_touch_mtime_ctime helper
	emptyHash := c.hasher.EmptyFileHash()
	if err := c.fs.SetHashSize(fpath, emptyHash, 0); err != nil {
		return fmt.Errorf("SetHashSize failed: %w", err)
	}

	t1, err := getInodeTimes(fpath)
	if err != nil {
		return err
	}

	if !t1.Mtime.After(t0.Mtime) {
		return fmt.Errorf("SetHashSize did not advance mtime! t0: %v, t1: %v", t0.Mtime, t1.Mtime)
	}
	if !t1.Ctime.After(t0.Ctime) {
		return fmt.Errorf("SetHashSize did not advance ctime! t0: %v, t1: %v", t0.Ctime, t1.Ctime)
	}

	return nil
}

// NegativeThenCreateTest verifies that a cached negative dentry does not
// shadow a subsequent create through the mount. casfs_revalidate() returns
// 1 for negatives outside .git/objects/xx, so this relies on the create path
// instantiating the dentry the dcache already holds.
func (c *casTest) NegativeThenCreateTest(dir string) error {
	if err := c.MkdirTest(dir); err != nil {
		return err
	}

	fpath := filepath.Join(dir, "appears-later.txt")

	if _, err := os.Stat(fpath); err == nil {
		return fmt.Errorf("file unexpectedly exists before create")
	} else if !os.IsNotExist(err) {
		return fmt.Errorf("expected ENOENT, got %v", err)
	}

	if err := c.FileTestContent(fpath, "created after negative lookup"); err != nil {
		return err
	}

	if _, err := os.Stat(fpath); err != nil {
		return fmt.Errorf("stale negative dentry: file invisible after create: %w", err)
	}

	if err := ValidateFileContent(fpath, "created after negative lookup"); err != nil {
		return err
	}

	if err := os.Remove(fpath); err != nil {
		return err
	}
	if _, err := os.Stat(fpath); !os.IsNotExist(err) {
		return fmt.Errorf("expected ENOENT after unlink, got %v", err)
	}

	return nil
}

func (c *casTest) NegativeThenSetHashTest(srcDir, dstDir string, probeNames ...string) error {
	if err := DirTestStage(srcDir); err != nil {
		return err
	}
	hash, err := c.fs.Hash(srcDir)
	if err != nil {
		return err
	}

	if err := MkdirTest(dstDir); err != nil {
		return err
	}

	//  Cache negatives for names the new tree WILL contain.
	for _, name := range probeNames {
		probe := filepath.Join(dstDir, name)
		if _, err := os.Stat(probe); !os.IsNotExist(err) {
			return fmt.Errorf("expected ENOENT on empty dir probe %q, got %v", name, err)
		}
	}

	// Replace the directory contents out of band.
	if err := c.fs.SetHash(dstDir, hash); err != nil {
		return err
	}

	// Every probed entry must now be visible.
	for _, name := range probeNames {
		probe := filepath.Join(dstDir, name)
		if _, err := os.Stat(probe); err != nil {
			return fmt.Errorf("stale negative dentry shadows %q after SetHash: %w", name, err)
		}
	}

	return ValidateTestStage(dstDir)
}
