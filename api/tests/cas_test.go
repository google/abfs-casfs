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
	"os"
	"strconv"
	"strings"
	"testing"

	"github.com/stretchr/testify/require"
)

func isKernelAtLeast(reqMajor, reqMinor int) bool {
	data, err := os.ReadFile("/proc/sys/kernel/osrelease")
	if err != nil {
		return false
	}
	parts := strings.Split(strings.TrimSpace(string(data)), ".")
	if len(parts) < 2 {
		return false
	}
	major, err1 := strconv.Atoi(parts[0])
	minor, err2 := strconv.Atoi(parts[1])
	if err1 != nil || err2 != nil {
		return false
	}
	if major > reqMajor {
		return true
	}
	return major == reqMajor && minor >= reqMinor
}

/*
 * Test a given CAS file system.
 *
 * Tests will check the hash value of files/directories/symlinks versus
 * empty/zero or that they have changed but not the actual hash values
 * these tests will be handled by the specific hasher tests
 *
 */
func TestCAS_Dir(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.MkdirTest(path)
	require.NoError(t, err)
}
func TestCAS_DirHash(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.MkdirTestStat(path)
	require.NoError(t, err)
}
func TestCAS_File(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.FileTestContent(path, "hash content")
	require.NoError(t, err)
}
func TestCAS_FileStat(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.FileTestContentStat(path, "hash content")
	require.NoError(t, err)
}
func TestCAS_FileTruncate(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.FileTestTruncate(path, "hash content")
	require.NoError(t, err)
}
func TestCAS_FileTruncateOpen(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.FileTestTruncateOpen(path, "hash content")
	require.NoError(t, err)
}
func TestCAS_FileCopy(t *testing.T) {
	src := env.CasTestNextPath()
	dst := env.CasTestNextPath()
	err := env.cas.FileTestCopy(src, dst, "copy file contents via cas")
	require.NoError(t, err)
}
func TestCAS_Symlink(t *testing.T) {
	src := env.CasTestNextPath()
	dst := env.CasTestNextPath()
	err := env.cas.SymlinkTest(src, dst)
	require.NoError(t, err)
}

func TestCAS_SymlinkStat(t *testing.T) {
	src := env.CasTestNextPath()
	dst := env.CasTestNextPath()
	err := env.cas.SymlinkTestStat(src, dst)
	require.NoError(t, err)
}
func TestCAS_SymlinkNoSourceStat(t *testing.T) {
	src := env.CasTestNextPath()
	dst := env.CasTestNextPath()
	err := env.cas.SymlinkTestNoSourceStat(src, dst)
	require.NoError(t, err)
}
func TestCAS_HardlinkFile(t *testing.T) {
	src := env.CasTestNextPath()
	dst := env.CasTestNextPath()
	err := env.cas.HardLinkFile(src, dst)
	require.NoError(t, err)
}
func TestCAS_DirTestStage(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.DirTestStage(path)
	require.NoError(t, err)
}
func TestCAS_DirTestStageCopy(t *testing.T) {
	srcPath := env.CasTestNextPath()
	cpPath := env.CasTestNextPath()
	err := env.cas.DirTestStageCopy(srcPath, cpPath)
	require.NoError(t, err)
}
func TestCAS_ExcludeDir(t *testing.T) {
	parent := env.CasTestNextPath()
	require.NoError(t, env.cas.ExcludeDirTest(parent, "test-dir"))
}
func TestCAS_ExcludeFile(t *testing.T) {
	parent := env.CasTestNextPath()
	require.NoError(t, env.cas.ExcludeFileTest(parent, "test-file"))
}
func TestCAS_Debug(t *testing.T) {
	dir := env.CasTestNextPath()
	require.NoError(t, env.cas.DebugTests(dir))
}
func TestCAS_Hashes(t *testing.T) {
	require.NoError(t, env.cas.HashTests())
}
func TestCAS_SnapshotBlockedByMmapWriting(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.SnapshotMmapWriteBlockTest(path)
	require.NoError(t, err)
}
func TestCAS_SeekReadPositionConsistency(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.SeekReadPositionConsistencyTest(path)
	require.NoError(t, err)
}

func TestCAS_AtomicAppendWrite(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.AtomicAppendWriteTest(path)
	require.NoError(t, err)
}
func TestCAS_ConcurrentSharedDescriptorStress(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.ConcurrentSharedDescriptorStressTest(path)
	require.NoError(t, err)
}

func TestCAS_TruncateReadInterleave(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.TruncateReadInterleaveTest(path)
	require.NoError(t, err)
}

func TestCAS_CollisionIdempotencyDoubleFree(t *testing.T) {
	p1 := env.CasTestNextPath() + "-dup1.txt"
	p2 := env.CasTestNextPath() + "-dup2.txt"
	err := env.cas.IdempotentCollisionDoubleFreeTest(p1, p2)
	require.NoError(t, err)
}

func TestCAS_NamespaceConcurrencyRace(t *testing.T) {
	testDir := env.CasTestNextPath()
	err := os.MkdirAll(testDir, 0755)
	require.NoError(t, err)
	err = env.cas.ConcurrencyNamespaceRaceTest(testDir)
	require.NoError(t, err)
}
func TestCAS_ProcPidMapsIsolation(t *testing.T) {
	/*
	 * Skip on kernels older than 6.6
	 */
	if !isKernelAtLeast(6, 6) {
		t.Skip("Skipping ProcPidMapsIsolation validation on pre-6.6 kernel environment")
	}
	basePath := env.CasTestNextPath()
	linkPath := env.CasTestNextPath()
	err := env.cas.ProcPidMapsIsolationTest(basePath, linkPath)
	require.NoError(t, err)
}

func TestCAS_SpliceStreamingMetadata(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.SpliceStreamingMetadataTest(path)
	require.NoError(t, err)
}

func TestCAS_RenameAtomicMetadataSwap(t *testing.T) {
	dir := env.CasTestNextPath()
	err := os.MkdirAll(dir, 0755)
	require.NoError(t, err)

	err = env.cas.RenameAtomicMetadataSwapTest(dir)
	require.NoError(t, err)
}

func TestCAS_ActiveWriteStreamInterleave(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.ActiveWriteStreamInterleaveTest(path)
	require.NoError(t, err)
}
func TestCAS_DynamicPromotionAppend(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.DynamicPromotionAppendTest(path)
	require.NoError(t, err)
}
func TestCAS_SharedInodeMultipleFdReadWritePromotion(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.SharedInodeMultipleFdReadWritePromotionTest(path)
	require.NoError(t, err)
}

func TestCAS_MmapInterleavePageFaultRace(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.MmapInterleavePageFaultRaceTest(path)
	require.NoError(t, err)
}

func TestCAS_SuddenEvictionDuringReadActive(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.SuddenEvictionDuringReadActiveTest(path)
	require.NoError(t, err)
}

func TestCAS_ConcurrentReadWriteIterStress(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.ConcurrentReadWriteIterStressTest(path)
	require.NoError(t, err)
}
func TestCAS_ActiveOpenCloseValidation(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.ActiveOpenCloseValidationTest(path)
	require.NoError(t, err)
}

func TestCAS_ConcurrentPromotionTruncateStressTest(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.ConcurrentPromotionTruncateStressTest(path)
	require.NoError(t, err)
}
func TestCAS_DirectBinaryExecutionUnmount(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.DirectBinaryExecutionUnmountTest(path)
	require.NoError(t, err)
}
func TestCAS_ARISize(t *testing.T) {
	ari := "test-ari-identifier-12345"
	payload := []byte("hello world payload from ari store")

	require.NoError(t, env.AddARIData(ari, payload))

	path := env.CasTestNextPath()
	err := env.cas.ARISizeTest(path, ari, int64(len(payload)), payload)
	require.NoError(t, err)
}

func TestCAS_ARIWriteAndSnapshot(t *testing.T) {
	ari := "test-ari-write-snapshot-999"
	origPayload := []byte("original content from ARI store")
	newPayload := []byte("brand new written data payload")
	require.NoError(t, env.AddARIData(ari, origPayload))
	path := env.CasTestNextPath()
	err := env.cas.ARIWriteAndSnapshotTest(path, ari, origPayload, newPayload)
	require.NoError(t, err)
}
func TestCAS_ARISetOnHashedFile(t *testing.T) {
	path := env.CasTestNextPath()
	ari := "test-ari-hashed-file-check"
	err := env.cas.ARISetOnHashedFileTest(path, "some file content", ari)
	require.NoError(t, err)
}

func TestCAS_ARISetSameARI(t *testing.T) {
	path := env.CasTestNextPath()
	ari := "test-ari-id-100"
	err := env.cas.ARISetSameARITest(path, ari, 1024)
	require.NoError(t, err)
}

func TestCAS_ARISetDifferentARI(t *testing.T) {
	path := env.CasTestNextPath()
	ari1 := "test-ari-original"
	ari2 := "test-ari-replacement"
	err := env.cas.ARISetDifferentARITest(path, ari1, ari2, 1024)
	require.NoError(t, err)
}

func TestCAS_ARISetWithColons(t *testing.T) {
	path := env.CasTestNextPath()
	ari := "rbe:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855:0"
	err := env.cas.ARISetSameARITest(path, ari, 0)
	require.NoError(t, err)
}

func TestCAS_ARISetHashOnARIFile(t *testing.T) {
	path := env.CasTestNextPath()
	ari := "test-ari-no-hash-override"
	err := env.cas.ARISetHashOnARIFileTest(path, ari, 1024)
	require.NoError(t, err)
}

func TestCAS_ARISetInvalidLength(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.ARISetInvalidLengthTest(path)
	require.NoError(t, err)
}

func TestCAS_BackendStatARI(t *testing.T) {
	ari := "test-ari-backend-stat-101"
	payload := []byte("hello world payload from backend stat ari test")

	require.NoError(t, env.AddARIData(ari, payload))

	path := env.CasTestNextPath()
	err := env.cas.BackendStatARITest(path, ari, int64(len(payload)), payload, 0644)
	require.NoError(t, err)
}

func TestCAS_BackendStatHashedFile(t *testing.T) {
	path := env.CasTestNextPath()
	content := "sample content block for backend stat hashed file test"

	err := env.cas.BackendStatHashedFileTest(path, content, 0755) // tested with executable bits
	require.NoError(t, err)
}

func TestCAS_BackendStatDirtyARIFile(t *testing.T) {
	ari := "test-ari-dirty-backend-stat-202"
	origPayload := []byte("original payload")
	newPayload := []byte("newly modified data payload")

	require.NoError(t, env.AddARIData(ari, origPayload))

	path := env.CasTestNextPath()
	err := env.cas.BackendStatDirtyARIFileTest(path, ari, origPayload, newPayload, 0600)
	require.NoError(t, err)
}
func TestCAS_ARIMissingProviderData(t *testing.T) {
	path := env.CasTestNextPath()
	// Use an ARI that has NOT been populated in the provider
	ari := "non-existent-ari-missing-payload-404"

	err := env.cas.ARIMissingProviderDataTest(path, ari, 1024)
	require.NoError(t, err)
}
func TestCAS_CopyFileRangeCleanDestination(t *testing.T) {
	src := env.CasTestNextPath()
	dst := env.CasTestNextPath()
	err := env.cas.CopyFileRangeCleanDestinationTest(src, dst)
	require.NoError(t, err)
}
func TestCAS_XattrHashUpdateTimestamps(t *testing.T) {
	path := env.CasTestNextPath()
	err := env.cas.XattrHashUpdateTimestampsTest(path)
	require.NoError(t, err)
}
func TestCAS_NegativeThenCreate(t *testing.T) {
	dir := env.CasTestNextPath()
	require.NoError(t, env.cas.NegativeThenCreateTest(dir))
}

func TestCAS_NegativeThenSetHash(t *testing.T) {
	src := env.CasTestNextPath()
	dst := env.CasTestNextPath()
	require.NoError(t, env.cas.NegativeThenSetHashTest(src, dst, "dir-0", "dir-3/file"))
}
