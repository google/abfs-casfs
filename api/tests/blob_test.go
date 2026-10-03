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

func TestBlob_LoadOnly(t *testing.T) {
	require.NoError(t, env.blob.LoadOnly(env.BlobTestNextPath()))
}

func TestBlob_ReadOnly(t *testing.T) {
	require.NoError(t, env.blob.ReadOnly(env.BlobTestNextPath(), env.BlobTestNextPath(), false, false))
}
func TestBlob_ReadOnlyOpenTruncate(t *testing.T) {
	require.NoError(t, env.blob.ReadOnly(env.BlobTestNextPath(), env.BlobTestNextPath(), true, false))
}
func TestBlob_ReadOnlyTruncate(t *testing.T) {
	require.NoError(t, env.blob.ReadOnly(env.BlobTestNextPath(), env.BlobTestNextPath(), false, true))
}
func TestBlob_WriteOnly(t *testing.T) {
	require.NoError(t, env.blob.WriteOnly(env.BlobTestNextPath(), env.BlobTestNextPath(), false))
}
func TestBlob_WriteOnlySnapshot(t *testing.T) {
	require.NoError(t, env.blob.WriteOnly(env.BlobTestNextPath(), env.BlobTestNextPath(), true))
}
func TestBlob_ReadWrite(t *testing.T) {
	require.NoError(t, env.blob.ReadWrite(env.BlobTestNextPath(), env.BlobTestNextPath(), false, false, false))
}
func TestBlob_ReadWriteOpenTruncate(t *testing.T) {
	require.NoError(t, env.blob.ReadWrite(env.BlobTestNextPath(), env.BlobTestNextPath(), false, true, false))
}
func TestBlob_ReadWriteTruncate(t *testing.T) {
	require.NoError(t, env.blob.ReadWrite(env.BlobTestNextPath(), env.BlobTestNextPath(), false, false, true))
}

func TestBlob_ReadWriteSnapshot(t *testing.T) {
	require.NoError(t, env.blob.ReadWrite(env.BlobTestNextPath(), env.BlobTestNextPath(), true, false, false))
}
func TestBlob_ReadWriteSnapshotOpenTruncate(t *testing.T) {
	require.NoError(t, env.blob.ReadWrite(env.BlobTestNextPath(), env.BlobTestNextPath(), true, true, false))
}
func TestBlob_ReadWriteSnapshotTruncate(t *testing.T) {
	require.NoError(t, env.blob.ReadWrite(env.BlobTestNextPath(), env.BlobTestNextPath(), true, false, true))
}

func TestBlob_WriteRead(t *testing.T) {
	require.NoError(t, env.blob.WriteRead(env.BlobTestNextPath(), env.BlobTestNextPath(), false, false, false))
}
func TestBlob_WriteReadOpenTruncate(t *testing.T) {
	require.NoError(t, env.blob.WriteRead(env.BlobTestNextPath(), env.BlobTestNextPath(), false, true, false))
}
func TestBlob_WriteReadTruncate(t *testing.T) {
	require.NoError(t, env.blob.WriteRead(env.BlobTestNextPath(), env.BlobTestNextPath(), false, false, true))
}

func TestBlob_WriteReadSnapshot(t *testing.T) {
	require.NoError(t, env.blob.WriteRead(env.BlobTestNextPath(), env.BlobTestNextPath(), true, false, false))
}
func TestBlob_WriteReadSnapshotOpenTruncate(t *testing.T) {
	require.NoError(t, env.blob.WriteRead(env.BlobTestNextPath(), env.BlobTestNextPath(), true, true, false))
}
func TestBlob_WriteReadSnapshotTruncate(t *testing.T) {
	require.NoError(t, env.blob.WriteRead(env.BlobTestNextPath(), env.BlobTestNextPath(), true, false, true))
}
