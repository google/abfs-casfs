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
	"crypto/sha1"
	"encoding/hex"
	"fmt"
	"os"
	"path"
	"sync/atomic"
	"syscall"

	"github.com/google/abfs-casfs/api"
	"github.com/google/abfs-casfs/api/hash"
	"github.com/google/abfs-casfs/api/xattr"
)

var testDirs = []string{"fs_test", "cas_test", "ctrl_test", "git_test", "tap_test", "blob_test"}

type Env struct {
	rootDir         string
	nonCasfsRootDir string
	mountObjDir     string
	mountBlobDir    string
	mountAriDir     string
	testDir         string
	nonCasfsTestDir string
	testDirs        map[string]string

	nextId atomic.Int32

	ctrl   *ctrlTest
	git    *gitTest
	cas    *casTest
	tap    *tapTest
	blob   *blobTest
	hasher api.Hasher
	cli    string
}

func (e *Env) CreateObjects() {
	e.hasher = hash.NewHasher()

	e.ctrl = &ctrlTest{fs: xattr.NewCASFS(e.rootDir, nil)}
	e.git = &gitTest{fs: xattr.NewCASFS(e.rootDir, nil), bin: "git"}
	e.cas = &casTest{fs: xattr.NewCASFS(e.rootDir, e.hasher), hasher: e.hasher}
	e.tap = &tapTest{fs: xattr.NewCASFS(e.rootDir, e.hasher), hasher: e.hasher, cli: e.cli}
	e.blob = &blobTest{fs: xattr.NewCASFS(e.rootDir, e.hasher), hasher: e.hasher}
}

func (e *Env) AddARIData(ari string, payload []byte) error {
	h := sha1.Sum([]byte(ari))
	hashHex := hex.EncodeToString(h[:])

	// Shard pathing matching cas_store: first 3 hex digits as subdirectory
	shardDir := path.Join(e.mountAriDir, hashHex[:3])
	if err := os.MkdirAll(shardDir, 0755); err != nil {
		return err
	}

	filePath := path.Join(shardDir, hashHex[3:])
	return os.WriteFile(filePath, payload, 0644)
}

func (e *Env) NextId() int32 {
	return e.nextId.Add(1)
}
func (e *Env) FsTestDir() string {
	return e.testDirs["fs_test"]
}
func (e *Env) CasTestDir() string {
	return e.testDirs["cas_test"]
}
func (e *Env) CtrlTestDir() string {
	return e.testDirs["ctrl_test"]
}
func (e *Env) GitTestDir() string {
	return e.testDirs["git_test"]
}
func (e *Env) TapTestDir() string {
	return e.testDirs["tap_test"]
}
func (e *Env) BlobTestDir() string {
	return e.testDirs["blob_test"]
}

// Helper functions to get unique path name for a test
func (e *Env) FsTestNextPath() string {
	return fmt.Sprintf("%s/fs-%d", e.FsTestDir(), e.NextId())
}
func (e *Env) CasTestNextPath() string {
	return fmt.Sprintf("%s/cas-%d", e.CasTestDir(), e.NextId())
}
func (e *Env) CtrlTestNextPath() string {
	return fmt.Sprintf("%s/ctrl-%d", e.CtrlTestDir(), e.NextId())
}
func (e *Env) GitTestNextPath() string {
	return fmt.Sprintf("%s/git-%d", e.GitTestDir(), e.NextId())
}
func (e *Env) TapTestNextPath() string {
	return fmt.Sprintf("%s/next-%d", e.TapTestDir(), e.NextId())
}
func (e *Env) BlobTestNextPath() string {
	return fmt.Sprintf("%s/next-%d", e.BlobTestDir(), e.NextId())
}
func AllTestSetup(rootDir, nonCasDir, cli string) (out *Env, err error) {
	_ = syscall.Umask(0)
	out = &Env{rootDir: rootDir,
		nonCasfsRootDir: nonCasDir,
		testDirs:        make(map[string]string),
		cli:             cli}
	defer func() {
		if err != nil {
			AllTestTearDown(out)
		}
	}()
	out.testDir, err = os.MkdirTemp(rootDir, "")
	if err != nil {
		return out, err
	}
	out.nonCasfsTestDir, err = os.MkdirTemp(nonCasDir, "")
	if err != nil {
		return out, err
	}
	perm := os.FileMode(0755)
	out.CreateObjects()
	createTestDir := func(key string) error {
		dir := path.Join(out.testDir, key)
		err := os.Mkdir(dir, perm)
		if err != nil {
			return err
		}
		out.testDirs[key] = dir
		return nil
	}
	for _, k := range testDirs {
		if err := createTestDir(k); err != nil {
			return nil, err
		}
	}
	return out, nil
}

func AllTestTearDown(e *Env) {
	removeIfValid := func(dir string) {
		if dir != "" {
			os.RemoveAll(dir)
		}
	}
	removeIfValid(e.testDir)
	removeIfValid(e.nonCasfsTestDir)
}
