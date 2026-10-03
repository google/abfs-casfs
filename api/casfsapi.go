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

package api

import (
	"io"
	"io/fs"
	"os/exec"
)

type Hash interface {
	String() string

	IsEmptyDir() bool
	IsEmptyFile() bool
	IsZero() bool
	Equals(rhs Hash) bool
	Read(io.Reader) (int, error)
	Write(io.Writer) (int, error)
}

type Hasher interface {
	NewHash(h string) (Hash, error)
	ZeroHash() Hash
	EmptyDirHash() Hash
	EmptyFileHash() Hash
	// Get the description of the hash represented by a string
	DescriptionOfHashString() string
}

type HashStat struct {
	Hash Hash
	Mode fs.FileMode
	Size int64
}
type Status struct {
	Ready bool // File system is fully mounted and can respond to queries
}

type BackendStat struct {
	IsARI bool
	ARI   string
	Hash  Hash
	Size  int64
	Mode  fs.FileMode
}

type FS interface {
	Dirty(path string) (bool, error)
	DirList(path string) (Hash, error)
	Loaded(path string) (bool, error)

	MountPath() string
	Excluded(path string) (bool, error)
	SetExcluded(path string, val bool) error

	BinHash(path string) ([]byte, error)
	Hash(path string) (Hash, error)
	SetHash(path string, h Hash) error

	BackendStat(path string) (*BackendStat, error)

	HashStat(path string) (*HashStat, error)

	HashSize(path string) (Hash, int64, error)
	SetHashSize(path string, h Hash, s int64) error

	ARISize(path string) (string, int64, error)
	SetARISize(path, uri string, s int64) error

	SetDeepTree(path string, h Hash) error

	Debug(path string) (bool, error)
	SetDebug(path string, val bool) error

	// Create control directory under path
	CreateControl(path, directory string) error
	// Create GIT project under given path
	CreateGit(path, directory, head string) error
	// Create GIT objects only under the given path
	CreateGitObjects(path, directory string) error
	// Create Git Indexs based on given worktree dir

	CreateGitIndex(path, directory, workTree, hash string) error

	// Tap the input command
	Tap(cmd *exec.Cmd) (*TapCmdResult, error)
	// Tap the input command and resolve the files when complete
	TapResolved(cmd *exec.Cmd) (*TapCmdResolvedResult, error)
	// Status of FS
	Status() (*Status, error)
}
