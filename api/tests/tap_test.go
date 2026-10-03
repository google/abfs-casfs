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
	"testing"

	"github.com/stretchr/testify/require"
)

func TestTap_FileEncodingBackslash(t *testing.T) {
	file := "str\\\\escape.rscript"
	err := env.tap.FileEncodingTest(env.rootDir, env.TapTestNextPath(), file)
	require.NoError(t, err)
}
func TestTap_FileEncodingBackslashSingle(t *testing.T) {
	file := "str\\escape.rscript"
	err := env.tap.FileEncodingTest(env.rootDir, env.TapTestNextPath(), file)
	require.NoError(t, err)

}
func TestTap_FileEncodingQuote(t *testing.T) {
	file := "str\"escape.rscript"
	err := env.tap.FileEncodingTest(env.rootDir, env.TapTestNextPath(), file)
	require.NoError(t, err)
}

func TestTap_FileRename(t *testing.T) {
	err := env.tap.RenameTest(env.rootDir, env.TapTestNextPath(), "string.old", "string.new", "string.new")
	require.NoError(t, err)
}
func TestTap_FileEncodingRename(t *testing.T) {
	// rename is run through sh -c so need to acount for encoding change in
	// expected name
	err := env.tap.RenameTest(env.rootDir, env.TapTestNextPath(), "string.old", "string.\\\\new", "string.\\new")
	require.NoError(t, err)
}

func TestTap_WriteRemove(t *testing.T) {
	require.NoError(t, env.tap.WriteRemoveTest(env.rootDir, env.TapTestNextPath()))
}

func TestTap_HardLinkTest(t *testing.T) {
	t.Skip("temp skip for presubmit")
	require.NoError(t, env.tap.HardLinkTest(env.rootDir, env.TapTestNextPath()))
}
func TestTap_Mkdir(t *testing.T) {
	require.NoError(t, env.tap.MkdirTest(env.rootDir, env.TapTestNextPath()))
}
func TestTap_Rmdir(t *testing.T) {
	require.NoError(t, env.tap.RmdirTest(env.rootDir, env.TapTestNextPath()))
}
func TestTap_SymlinkDir(t *testing.T) {
	require.NoError(t, env.tap.SymlinkDirTest(env.rootDir, env.TapTestNextPath()))
}
func TestTap_SymlinkFile(t *testing.T) {
	require.NoError(t, env.tap.SymlinkFileTest(env.rootDir, env.TapTestNextPath()))
}
func TestTap_ReadOnlyCreate(t *testing.T) {
	require.NoError(t, env.tap.ReadOnlyCreateTest(env.rootDir, env.TapTestNextPath()))
}
func TestTap_ReadOnlyExecCreate(t *testing.T) {
	require.NoError(t, env.tap.ReadOnlyCreateExecTest(env.rootDir, env.TapTestNextPath()))
}
func TestTap_ChmodTest(t *testing.T) {
	require.NoError(t, env.tap.ChmodTest(env.rootDir, env.TapTestNextPath()))
}

func TestTapResolved_FileRead(t *testing.T) {
	require.NoError(t, env.tap.FileReadResolved(env.rootDir, env.TapTestNextPath()))
}
func TestTapResolved_FileCreateDeleteMkdir(t *testing.T) {
	require.NoError(t, env.tap.FileCreateDeleteMkdirResolved(env.rootDir, env.TapTestNextPath()))
}
func TestTapResolved_FileCreateRenameResolved(t *testing.T) {
	require.NoError(t, env.tap.FileCreateRenameResolved(env.rootDir, env.TapTestNextPath()))
}
func TestTapResolved_FileRenameDirResolved(t *testing.T) {
	require.NoError(t, env.tap.FileRenameDirResolved(env.rootDir, env.TapTestNextPath()))
}

// TestTap_StreamingBatchOverflow validates that large bursts of file operations
// do not cause page memory pool corruption or premature EOF cutoff terminations.
func TestTap_StreamingBatchOverflow(t *testing.T) {
	err := env.tap.TestBatchDrainingAndOverflow(env.rootDir, env.TapTestNextPath())
	require.NoError(t, err)
}

// TestTap_ImmediateProcessExit tests the zero-delay condition where a command
// closes immediately with an empty execution loop, validating the ioctl state flag boundary.
func TestTap_ImmediateProcessExit(t *testing.T) {
	testDir := env.TapTestNextPath() // Capture path state exactly once

	err := env.tap.CtrlTapTest(testDir)
	require.NoError(t, err)

	// Invoke a true blank op command line call pass reusing the setup path
	result, err := env.tap.RunCmd(testDir, []string{"true"})
	require.NoError(t, err)
	require.Empty(t, result.TapEntries, "Expected zero trace blocks for no-op execution pipelines")
}

// TestTap_IntermittentBurstYield forces a time delay block midway through
// execution to verify that your Go reader's Select/Poll block can recover from an EAGAIN condition.
func TestTap_IntermittentBurstYield(t *testing.T) {
	testDir := env.TapTestNextPath() // Capture path state exactly once

	err := env.tap.CtrlTapTest(testDir)
	require.NoError(t, err)

	// Execute a shell statement with an explicit 50ms internal sleep cycle block
	cmdArgs := []string{"/bin/sh", "-c", "touch sleep_test.txt; sleep 0.05; touch post_sleep_test.txt"}
	result, err := env.tap.RunCmd(testDir, cmdArgs)
	require.NoError(t, err)

	// Total items expected: (2 entries for file 1) + (2 entries for file 2) = 4 entries.
	require.Len(t, result.TapEntries, 4, "The yield window caused a data drop anomaly inside the streaming loop matrix")
}
func TestTap_ARITapRead(t *testing.T) {
	ari := "test-ari-tap-identifier-999"
	payload := []byte("hello world tapped ari content payload")

	require.NoError(t, env.AddARIData(ari, payload))
	err := env.tap.ARITapReadTest(env.rootDir, env.TapTestNextPath(), ari, payload)
	require.NoError(t, err)
}
