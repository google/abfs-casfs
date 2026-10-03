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

package hash

import (
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"

	"github.com/google/abfs-casfs/api"
)

const (
	// Size defines the amount of bytes the hash yields.
	hashSize = 20
	// HexSize defines the strings size of the hash when represented in hexadecimal.
	hashHexSize = 40
)

type hash [hashSize]byte

var emptyFileHash hash
var emptyDirHash hash

func init() {
	_, _ = hex.Decode(emptyFileHash[:], []byte("e69de29bb2d1d6434b8b29ae775ad8c2e48c5391"))
	_, _ = hex.Decode(emptyDirHash[:], []byte("4b825dc642cb6eb9a060e54bf8d69288fbee4904"))
}

func (h *hash) String() string {
	return hex.EncodeToString(h[:])
}
func (h *hash) IsEmptyDir() bool {
	return *h == emptyDirHash
}
func (h *hash) IsEmptyFile() bool {
	return *h == emptyFileHash
}
func (h *hash) IsZero() bool {
	return *h == hash{}
}
func (h *hash) Equals(rhs api.Hash) bool {
	return *h == *rhs.(*hash)
}
func (h *hash) Write(w io.Writer) (int, error) {
	return w.Write(h[:])
}
func (h *hash) Read(r io.Reader) (int, error) {
	return io.ReadFull(r, h[:])
}
func (h *hash) MarshalJSON() ([]byte, error) {
	return json.Marshal(h.String())
}

type hasher struct {
}

func NewHasher() api.Hasher {
	return &hasher{}
}
func (h *hasher) NewHash(s string) (api.Hash, error) {
	if len(s) != hashHexSize {
		return nil, fmt.Errorf("invalid hash string len %d", len(s))
	}
	b, err := hex.DecodeString(s)
	if err != nil {
		return nil, err
	}
	var hash hash
	copy(hash[:], b)

	return &hash, nil
}
func (h *hasher) ZeroHash() api.Hash {
	return &hash{}
}
func (h *hasher) EmptyFileHash() api.Hash {
	out := emptyFileHash
	return &out
}
func (h *hasher) EmptyDirHash() api.Hash {
	out := emptyDirHash
	return &out
}
func (h *hasher) DescriptionOfHashString() string {
	return "40 character SHA-1 string"
}
