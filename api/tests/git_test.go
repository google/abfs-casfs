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
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/stretchr/testify/require"
)

/*
 * test control interface
 */
var nonCasGitHead string

func gitTestInit(nonCasTestRoot, objectsDir string) error {

	baseline := filepath.Join(nonCasTestRoot, "baseline")
	if err := env.git.CreateNonCasRepo(baseline); err != nil {
		return fmt.Errorf("failed creating non-cas repo %v", err)
	}
	// Copy objects into our object directory for tests
	if err := TxObjects(baseline, objectsDir); err != nil {
		return fmt.Errorf("failed to tx cobjects %v", err)
	}
	head, err := env.git.GetHead(baseline)
	if err != nil {
		return fmt.Errorf("failed to get head %v", err)
	}
	nonCasGitHead = head
	return nil
}

// Setup git via control
func TestGit_Control(t *testing.T) {
	path := env.GitTestNextPath()
	err := env.git.CtrlGitTest(path, nonCasGitHead)
	require.NoError(t, err)
}
func TestGit_ControlRm(t *testing.T) {
	path := env.GitTestNextPath()
	err := env.git.CtrlGitRmTest(path, nonCasGitHead)
	require.NoError(t, err)
}

// Consider deprecating if no longer in use
func TestGit_ControlObj(t *testing.T) {
	path := env.GitTestNextPath()
	// give a different name than objects to test sw
	err := env.git.CtrlGitObjTest(path, "git_objects")
	require.NoError(t, err)
}

func TestGit_ControlObjRm(t *testing.T) {
	path := env.GitTestNextPath()
	// give a different name than objects to test sw
	err := env.git.CtrlGitObjRmTest(path, "git_objects")
	require.NoError(t, err)
}

// The gitindex is mainly used for spliting .git and source tree
func TestGit_ControlIndex(t *testing.T) {
	dotGitParent := env.GitTestNextPath()
	workTree := env.GitTestNextPath()
	mntRoot := env.rootDir
	require.NoError(t, env.git.CtrlGitIndexTest(mntRoot, dotGitParent, workTree, nonCasGitHead))
}

func TestGit_ControlIndexPathCheck(t *testing.T) {
	dotGitParent := env.GitTestNextPath()
	workTree := filepath.Join(dotGitParent, "worktree")
	mntRoot := env.rootDir
	require.NoError(t, env.git.CtrlGitIndexTest(mntRoot, dotGitParent, workTree, nonCasGitHead))
}

func TestGit_Head(t *testing.T) {
	path := env.GitTestNextPath()
	err := env.git.HeadTest(path, nonCasGitHead)
	require.NoError(t, err)
}
func TestGit_Show(t *testing.T) {
	path := env.GitTestNextPath()
	err := env.git.ShowTest(path, nonCasGitHead)
	require.NoError(t, err)
}
func TestGit_Commit(t *testing.T) {
	path := env.GitTestNextPath()
	err := env.git.CommitTest(path, nonCasGitHead)
	require.NoError(t, err)
}

func gitSetup(srcPath string) error {
	if err := MkdirTest(srcPath); err != nil {
		return err
	}

	// Mark .git as excluded from CAS tracking
	gitDir := filepath.Join(srcPath, ".git")
	if err := MkdirTest(gitDir); err != nil {
		return err
	}

	if err := env.cas.fs.SetExcluded(gitDir, true); err != nil {
		return err
	}

	return env.git.PopulateNonCasRepo(srcPath)
}
func TestGit_Tree(t *testing.T) {
	srcPath := env.GitTestNextPath()
	err := gitSetup(srcPath)
	require.NoError(t, err)

	// Get the tree hash and tx the objects
	err = TxObjects(srcPath, env.mountObjDir)
	require.NoError(t, err)

	head, err := env.git.GetHead(srcPath)
	require.NoError(t, err)
	hash, err := env.cas.fs.Hash(srcPath)
	require.NoError(t, err)

	// Create new directory with the git data
	path := env.GitTestNextPath()
	err = MkdirTest(path)
	require.NoError(t, err)
	err = env.cas.fs.SetHash(path, hash)
	require.NoError(t, err)
	// setup the git
	err = env.git.fs.CreateGit(path, ".git", head)
	require.NoError(t, err)

	indexBefore, err := os.ReadFile(filepath.Join(path, ".git", "index"))
	require.NoError(t, err)
	_, err = env.git.GitStatus(path)
	require.NoError(t, err)

	indexAfter, err := os.ReadFile(filepath.Join(path, ".git", "index"))
	require.NoError(t, err)
	require.Equal(t, indexBefore, indexAfter)

}

func TestGit_StatBlob(t *testing.T) {
	srcPath := env.GitTestNextPath()
	err := gitSetup(srcPath)
	require.NoError(t, err)

	blobs, err := env.git.AllBlobs(srcPath)
	require.NoError(t, err)

	// Get the tree hash and tx the objects
	err = TxObjects(srcPath, env.mountObjDir)
	require.NoError(t, err)

	head, err := env.git.GetHead(srcPath)
	require.NoError(t, err)
	hash, err := env.cas.fs.Hash(srcPath)
	require.NoError(t, err)

	// Create new directory with the git data
	path := env.GitTestNextPath()
	err = MkdirTest(path)
	require.NoError(t, err)
	err = env.cas.fs.SetHash(path, hash)
	require.NoError(t, err)
	// setup the git
	err = env.git.fs.CreateGit(path, ".git", head)
	require.NoError(t, err)

	for _, blob := range blobs {
		dirname, filename := blob[:2], blob[2:]
		objPath := filepath.Join(path, ".git", "objects", dirname, filename)
		stat, err := os.Stat(objPath)
		require.NoError(t, err)

		cachePath := filepath.Join(env.mountBlobDir, blob[:3], blob[3:])
		cStat, err := os.Stat(cachePath)
		require.NoError(t, err)
		require.Equal(t, stat.Size(), zlib_compress_size(cStat.Size()))

		_, err = env.git.CatBlob(path, blob)
		require.NoError(t, err)
	}
}

func zlib_compress_size(size int64) int64 {
	block_size := int64(65535)
	content_size := 6 + size + int64(len(fmt.Sprintf("%d", size)))
	block_num := (content_size + block_size - 1) / block_size
	overhead_size := 6 + (5 * block_num)
	return content_size + overhead_size
}

func TestGit_LFSLikeAccess(t *testing.T) {
	path := env.GitTestNextPath()
	err := env.git.LFSLikeAccess(path, nonCasGitHead)
	require.NoError(t, err)
}
func TestGit_IndexValidation(t *testing.T) {
	dotGitParent := env.GitTestNextPath()
	workTree := env.GitTestNextPath()

	// Set up .git directory and HEAD
	require.NoError(t, env.git.CtrlGitTest(dotGitParent, nonCasGitHead))

	// Remove default index so CreateGitIndex can write a fresh one
	indexPath := filepath.Join(dotGitParent, ".git", "index")
	_ = os.Remove(indexPath)

	// Create work tree directory
	require.NoError(t, MkdirTest(workTree))

	// Generate index using CreateGitIndex
	require.NoError(t, env.git.fs.CreateGitIndex(dotGitParent, ".git", workTree, nonCasGitHead))

	// Verify the index file was generated on disk and is non-empty
	info, err := os.Stat(indexPath)
	require.NoError(t, err, "index file should exist")
	require.Greater(t, info.Size(), int64(0), "index file should not be zero bytes")

	// Run git status to ensure Git parses the generated index headers & extensions without format errors
	_, stderr, err := env.git.RunCmdInDir(dotGitParent, "status", "--short")
	require.NoError(t, err, fmt.Sprintf("git status failed on index: %s", strings.Join(stderr, "\n")))
}

func TestGit_DeepTreeIndexGeneration(t *testing.T) {
	path := env.GitTestNextPath()

	// Initialize CASFS git environment with nonCasGitHead
	require.NoError(t, env.git.CtrlGitTest(path, nonCasGitHead))

	// Verify HEAD matches expected
	headBack, err := env.git.GetHead(path)
	require.NoError(t, err)
	require.Equal(t, nonCasGitHead, headBack)

	// Verify git show succeeds on the head commit
	_, err = env.git.GitShow(path)
	require.NoError(t, err)
}
func TestGit_LongPathPaddingIndex(t *testing.T) {
	srcPath := env.GitTestNextPath()
	require.NoError(t, gitSetup(srcPath))

	// Test boundary path lengths to trigger edge cases in 8-byte padding
	for _, nameLen := range []int{1, 7, 8, 12, 63, 64, 128, 250} {
		fileName := strings.Repeat("a", nameLen) + ".txt"
		require.NoError(t, FileTestContent(filepath.Join(srcPath, fileName), "content"))
	}

	require.NoError(t, env.git.AddAll(srcPath))
	_, err := env.git.GitCommit(srcPath, "Add files with varying name lengths")
	require.NoError(t, err)

	require.NoError(t, TxObjects(srcPath, env.mountObjDir))

	head, err := env.git.GetHead(srcPath)
	require.NoError(t, err)

	path := env.GitTestNextPath()
	require.NoError(t, env.git.CtrlGitTest(path, head))

	// Verify git status parses every path and padding boundary cleanly
	_, err = env.git.GitStatus(path)
	require.NoError(t, err)
}
func TestGit_LargeFileCompressedHash(t *testing.T) {
	path := env.GitTestNextPath()
	require.NoError(t, env.git.CtrlGitTest(path, nonCasGitHead))

	// Write a file larger than MAX_HEADER_SIZE and PAGE_SIZE (e.g. 5MB)
	// to force multi-pass zlib deflation and kernel chunk writes
	largeFilePath := filepath.Join(path, "large_file.bin")
	largeData := bytes.Repeat([]byte("0123456789abcdef"), 1024*300) // ~4.8 MB

	require.NoError(t, os.WriteFile(largeFilePath, largeData, 0644))
	require.NoError(t, env.git.Add(path, largeFilePath))

	_, err := env.git.GitCommit(path, "Commit large compressed file")
	require.NoError(t, err)

	// Ensure the repo passes full integrity check
	_, err = env.git.GitFsck(path)
	require.NoError(t, err)
}

// Verify that empty directories in the worktree are not counted as subtrees
// and do not corrupt the cache-tree extension during user-space index creation.
func TestGit_EmptyDirectoryPruningInIndex(t *testing.T) {
	dotGitParent := env.GitTestNextPath()
	workTree := env.GitTestNextPath()

	require.NoError(t, env.git.CtrlGitTest(dotGitParent, nonCasGitHead))
	require.NoError(t, MkdirTest(workTree))

	// Create files mixed with empty subdirectories and nested empty directories
	require.NoError(t, FileTestContent(filepath.Join(workTree, "root.txt"), "hello"))
	require.NoError(t, os.MkdirAll(filepath.Join(workTree, "empty_dir"), 0755))
	require.NoError(t, os.MkdirAll(filepath.Join(workTree, "nested", "empty_child"), 0755))
	require.NoError(t, os.MkdirAll(filepath.Join(workTree, "nested", "valid_child"), 0755))
	require.NoError(t, FileTestContent(filepath.Join(workTree, "nested", "valid_child", "file.txt"), "data"))

	indexPath := filepath.Join(dotGitParent, ".git", "index")
	_ = os.Remove(indexPath)

	// Generate the index in user-space
	require.NoError(t, env.git.fs.CreateGitIndex(dotGitParent, ".git", workTree, nonCasGitHead))

	// 1. git status must read the index cleanly without errors
	stdout, stderr, err := env.git.RunCmdInDir(dotGitParent, "status", "--short")
	require.NoError(t, err, fmt.Sprintf("git status failed: %s\n%s", strings.Join(stdout, "\n"), strings.Join(stderr, "\n")))

	// 2. git write-tree verifies Git's cache-tree parser validates the TREE extension
	stdout, stderr, err = env.git.RunCmdInDir(dotGitParent, "write-tree")
	require.NoError(t, err, fmt.Sprintf("git write-tree failed: %s\n%s", strings.Join(stdout, "\n"), strings.Join(stderr, "\n")))
}

// Verify that multi-level nested directories serialize in strict pre-order depth-first order
// matching Git's cache_tree_read expectations.
func TestGit_PreOrderCacheTreeWriteTree(t *testing.T) {
	dotGitParent := env.GitTestNextPath()
	workTree := env.GitTestNextPath()

	require.NoError(t, env.git.CtrlGitTest(dotGitParent, nonCasGitHead))
	require.NoError(t, MkdirTest(workTree))

	// Create a multi-tier hierarchy:
	// a/b/file1.txt
	// a/c/file2.txt
	// d/file3.txt
	paths := []string{
		filepath.Join("a", "b", "file1.txt"),
		filepath.Join("a", "c", "file2.txt"),
		filepath.Join("d", "file3.txt"),
	}
	for _, rel := range paths {
		full := filepath.Join(workTree, rel)
		require.NoError(t, os.MkdirAll(filepath.Dir(full), 0755))
		require.NoError(t, FileTestContent(full, "content"))
	}

	indexPath := filepath.Join(dotGitParent, ".git", "index")
	_ = os.Remove(indexPath)

	require.NoError(t, env.git.fs.CreateGitIndex(dotGitParent, ".git", workTree, nonCasGitHead))

	// Ensure git diff-index and write-tree pass without cache-tree desync
	stdout, stderr, err := env.git.RunCmdInDir(dotGitParent, "write-tree")
	require.NoError(t, err, fmt.Sprintf("git write-tree failed on pre-order hierarchy: %s\n%s", strings.Join(stdout, "\n"), strings.Join(stderr, "\n")))
}

// Verify critical padding boundaries where (62 + len(path)) % 8 == 0 (requires 8 null padding bytes).
func TestGit_IndexPaddingBoundaries(t *testing.T) {
	dotGitParent := env.GitTestNextPath()
	workTree := env.GitTestNextPath()

	require.NoError(t, env.git.CtrlGitTest(dotGitParent, nonCasGitHead))
	require.NoError(t, MkdirTest(workTree))

	// Names selected to hit exact boundary conditions:
	// len("ab") == 2    => 62 + 2 = 64 (mod 8 == 0, must append 8 bytes padding)
	// len("abc") == 3   => 62 + 3 = 65 (mod 8 == 1, must append 7 bytes padding)
	// len("a...10") == 10 => 62 + 10 = 72 (mod 8 == 0, must append 8 bytes padding)
	boundaryFiles := []string{
		"ab",
		"abc",
		"1234567890",
		filepath.Join("dir", "ab"),
	}

	for _, f := range boundaryFiles {
		full := filepath.Join(workTree, f)
		require.NoError(t, os.MkdirAll(filepath.Dir(full), 0755))
		require.NoError(t, FileTestContent(full, "test"))
	}

	indexPath := filepath.Join(dotGitParent, ".git", "index")
	_ = os.Remove(indexPath)

	require.NoError(t, env.git.fs.CreateGitIndex(dotGitParent, ".git", workTree, nonCasGitHead))

	// git ls-files --stage verifies Git can parse all records past the boundary padding
	stdout, stderr, err := env.git.RunCmdInDir(dotGitParent, "ls-files", "--stage")
	require.NoError(t, err, fmt.Sprintf("ls-files failed on padding boundary: %s", strings.Join(stderr, "\n")))
	require.Equal(t, len(boundaryFiles), len(stdout), "all boundary files should be parsed by git")
}
func TestGit_NegativeLookupRetriedAfterProviderGainsObject(t *testing.T) {
	srcPath := env.GitTestNextPath()
	require.NoError(t, gitSetup(srcPath))

	// Create an object that is deliberately NOT transferred to the cache yet.
	require.NoError(t, FileTestContent(filepath.Join(srcPath, "late.txt"), "late arrival"))
	require.NoError(t, env.git.AddAll(srcPath))
	_, err := env.git.GitCommit(srcPath, "object the provider does not have yet")
	require.NoError(t, err)

	blobs, err := env.git.AllBlobs(srcPath)
	require.NoError(t, err)
	require.NotEmpty(t, blobs)
	late := blobs[len(blobs)-1]

	// Transfer everything EXCEPT the late object, so the tree is usable.
	require.NoError(t, TxObjects(srcPath, env.mountObjDir))
	require.NoError(t, os.RemoveAll(filepath.Join(env.mountObjDir, late[:3], late[3:])))

	head, err := env.git.GetHead(srcPath)
	require.NoError(t, err)

	path := env.GitTestNextPath()
	require.NoError(t, env.git.CtrlGitTest(path, head))

	objPath := env.git.ObjectPath(path, late)

	_, err = os.Stat(objPath)
	require.Error(t, err, "object should not be visible before the provider has it")
	require.True(t, os.IsNotExist(err), "expected ENOENT, got %v", err)

	require.NoError(t, TxOneObject(srcPath, env.mountObjDir, late))

	_, err = os.Stat(objPath)
	require.NoError(t, err, "negative dentry was cached permanently; lookup never retried the provider")

	_, err = env.git.CatBlob(path, late)
	require.NoError(t, err)
}

// Verify that objects created through the link/rename path are found by a
// subsequent lookup rather than a second node being built for the same name.
func TestGit_CreatedObjectVisibleAfterCommit(t *testing.T) {
	path := env.GitTestNextPath()
	require.NoError(t, env.git.CtrlGitTest(path, nonCasGitHead))

	require.NoError(t, FileTestContent(filepath.Join(path, "created.txt"), "fresh object"))
	require.NoError(t, env.git.Add(path, filepath.Join(path, "created.txt")))
	_, err := env.git.GitCommit(path, "create object through link/rename")
	require.NoError(t, err)

	blobs, err := env.git.AllBlobs(path)
	require.NoError(t, err)
	require.NotEmpty(t, blobs)

	// Every object git just wrote must be reachable by path and by content.
	// Stat twice: the first lookup after instantiate is the one that used to
	// miss the dcache and build a duplicate node.
	for _, blob := range blobs {
		objPath := env.git.ObjectPath(path, blob)

		st1, err := os.Stat(objPath)
		require.NoError(t, err, "object %s not visible after commit", blob)

		st2, err := os.Stat(objPath)
		require.NoError(t, err, "object %s vanished on second lookup", blob)
		require.Equal(t, st1.Size(), st2.Size(),
			"object %s changed size between lookups (duplicate node?)", blob)

		_, err = env.git.CatBlob(path, blob)
		require.NoError(t, err, "object %s unreadable after commit", blob)
	}

	// fsck walks every object through the same lookup path.
	_, err = env.git.GitFsck(path)
	require.NoError(t, err)
}
