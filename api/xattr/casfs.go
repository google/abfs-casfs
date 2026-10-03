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

package xattr

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/google/abfs-casfs/api"
	"github.com/google/abfs-casfs/api/gitindex"
	"github.com/pkg/xattr"
)

const (
	binHashAttrName     = "casfs.bin_hash"
	hashAttrName        = "casfs.hash"
	hashSizeAttrName    = "casfs.hash_size"
	ariSizeAttrName     = "casfs.ari_size"
	hashStatAttrName    = "casfs.hash_stat"
	backendStatAttrName = "casfs.backend_stat"
	dirListHashAttrName = "casfs.dir_list_hash"
	deepTreeAttrName    = "casfs.deeptree"
	ctrlAttrName        = "casfs.ctrl"
	debugAttrName       = "casfs.debug"
	dirtyAttrName       = "casfs.dirty"
	diagsAttrName       = "casfs.diags"
	loadedAttrName      = "casfs.loaded"
	excludeAttrName     = "casfs.exclude"
	fsStatusAttrName    = "casfs.fs_status"

	// The standard Git config we extracted from the kernel
	gitConfigString = `[core]
	compression = 0
	looseCompression = 0
	abbrev = no

[fetch]
	unpackLimit = 2147483647
[maintenance.loose-objects]
	auto = 0

[maintenance.incremental-repack]
	auto = 0
[casfs]
	noObjectCompress = true
	flatObjectPath = true
`
)

// Implement api.FS via xattr
type xattrFs struct {
	hasher    api.Hasher
	mountPath string
	tapPath   string
}

func NewCASFS(mountPath string, hasher api.Hasher) api.FS {
	return &xattrFs{hasher: hasher, mountPath: mountPath}
}

// Need to bind tap path as for tapping as we don't want tap path
// as part of the api interface
func SetTapPath(fs api.FS, tapPath string) {
	fs.(*xattrFs).tapPath = tapPath
}

func (fs *xattrFs) readByteArray(path, attr string) ([]byte, error) {
	data, err := xattr.LGet(path, attr)
	if err != nil {
		return []byte{}, err
	}
	return data, nil
}
func (fs *xattrFs) MountPath() string {
	return fs.mountPath
}
func (fs *xattrFs) readString(path, attr string) (string, error) {
	data, err := xattr.LGet(path, attr)
	if err != nil {
		return "", err
	}
	return string(data), nil
}

func (fs *xattrFs) writeString(path, attr, val string) error {
	err := xattr.LSet(path, attr, []byte(val))
	return err

}
func (fs *xattrFs) readBool(path, attr string) (bool, error) {
	val, err := fs.readString(path, attr)
	if err != nil {
		return false, err
	}
	if val == "0" {
		return false, nil
	}
	if val == "1" {
		return true, nil
	}
	return false, fmt.Errorf("invalid attr value %s", val)
}
func (fs *xattrFs) writeBool(path, attr string, val bool) error {
	var valString string
	if val {
		valString = "1"
	} else {
		valString = "0"
	}
	return fs.writeString(path, attr, valString)

}
func (fs *xattrFs) readBinHash(path, attr string) ([]byte, error) {
	val, err := fs.readByteArray(path, attr)
	if err != nil {
		return nil, err
	}
	return val, err
}
func (fs *xattrFs) readHash(path, attr string) (api.Hash, error) {
	val, err := fs.readString(path, attr)
	if err != nil {
		return nil, err
	}
	return fs.hasher.NewHash(val)
}
func (fs *xattrFs) writeHash(path, attr string, h api.Hash) error {
	return fs.writeString(path, attr, h.String())
}
func (fs *xattrFs) readHashSize(path, attr string) (api.Hash, int64, error) {

	val, err := fs.readString(path, attr)
	if err != nil {
		return nil, 0, err
	}
	parts := strings.SplitN(val, ":", 2)
	if len(parts) != 2 {
		return nil, 0, fmt.Errorf("invalid hash size string %s", val)
	}

	h, err := fs.hasher.NewHash(parts[0])
	if err != nil {
		return nil, 0, err
	}
	size, err := strconv.ParseInt(parts[1], 10, 64)
	if err != nil {
		return nil, 0, err
	}
	return h, size, nil
}
func (fs *xattrFs) readARISize(path, attr string) (string, int64, error) {

	val, err := fs.readString(path, attr)
	if err != nil {
		return "", 0, err
	}
	lastColon := strings.LastIndex(val, ":")
	if lastColon == -1 {
		return "", 0, fmt.Errorf("invalid ari size string %s", val)
	}

	size, err := strconv.ParseInt(val[lastColon+1:], 10, 64)
	if err != nil {
		return "", 0, err
	}
	return val[:lastColon], size, nil
}
func (fs *xattrFs) readBackendStat(path, attr string) (*api.BackendStat, error) {

	val, err := fs.readString(path, attr)
	if err != nil {
		return nil, err
	}
	// Format from kernel: "ari:<ARI_STRING>:<SIZE>:<MODE>" or "hash:<HEX_HASH>:<SIZE>:<MODE>"
	parts := strings.Split(val, ":")
	if len(parts) < 4 {

		return nil, fmt.Errorf("invalid backend stat string %s", val)
	}

	rawMode, err := strconv.ParseInt(parts[len(parts)-1], 10, 32)
	if err != nil {
		return nil, fmt.Errorf("cannot parse mode %q: %w", parts[len(parts)-1], err)
	}

	size, err := strconv.ParseInt(parts[len(parts)-2], 10, 64)
	if err != nil {
		return nil, fmt.Errorf("cannot parse size %q: %w", parts[len(parts)-2], err)
	}
	mode := api.OSModeToFSMode(uint32(rawMode))
	ident := strings.Join(parts[1:len(parts)-2], ":")

	switch parts[0] {
	case "ari":
		return &api.BackendStat{
			IsARI: true,
			ARI:   ident,
			Size:  size,
			Mode:  mode,
		}, nil
	case "hash":
		h, err := fs.hasher.NewHash(ident)
		if err != nil {
			return nil, err
		}
		return &api.BackendStat{
			IsARI: false,
			Hash:  h,
			Size:  size,
			Mode:  mode,
		}, nil
	default:
		return nil, fmt.Errorf("unknown backend stat type %s", parts[0])
	}
}
func (fs *xattrFs) readHashStat(path, attr string) (*api.HashStat, error) {
	val, err := fs.readString(path, attr)
	if err != nil {
		return nil, err
	}
	// Split hash:size:mode
	parts := strings.SplitN(val, ":", 3)
	if len(parts) != 3 {
		return nil, fmt.Errorf("invalid hash stat string %s", val)
	}
	h, err := fs.hasher.NewHash(parts[0])
	if err != nil {
		return nil, err
	}
	size, err := strconv.ParseInt(parts[1], 10, 64)
	if err != nil {
		return nil, err
	}

	mode, err := strconv.ParseInt(parts[2], 10, 32)
	if err != nil {
		return nil, err
	}

	out := &api.HashStat{Hash: h,
		Size: size,
		Mode: api.OSModeToFSMode(uint32(mode))}

	return out, nil
}
func (fs *xattrFs) writeHashSize(path, attr string, h api.Hash, size int64) error {
	return fs.writeString(path, attr, fmt.Sprintf("%s:%d", h.String(), size))
}
func (fs *xattrFs) writeARISize(path, attr, ari string, size int64) error {
	return fs.writeString(path, attr, fmt.Sprintf("%s:%d", ari, size))
}
func (fs *xattrFs) Dirty(path string) (bool, error) {
	return fs.readBool(path, dirtyAttrName)
}
func (fs *xattrFs) DirList(path string) (api.Hash, error) {
	return fs.readHash(path, dirListHashAttrName)
}
func (fs *xattrFs) Diags(path string) (string, error) {
	return fs.readString(path, diagsAttrName)
}
func (fs *xattrFs) Loaded(path string) (bool, error) {
	return fs.readBool(path, loadedAttrName)
}
func (fs *xattrFs) Excluded(path string) (bool, error) {
	return fs.readBool(path, excludeAttrName)
}

func (fs *xattrFs) SetExcluded(path string, val bool) error {
	return fs.writeBool(path, excludeAttrName, val)
}

func (fs *xattrFs) BinHash(path string) ([]byte, error) {
	return fs.readBinHash(path, binHashAttrName)
}
func (fs *xattrFs) Hash(path string) (api.Hash, error) {
	return fs.readHash(path, hashAttrName)
}
func (fs *xattrFs) SetHash(path string, h api.Hash) error {
	return fs.writeHash(path, hashAttrName, h)
}

func (fs *xattrFs) HashSize(path string) (api.Hash, int64, error) {
	return fs.readHashSize(path, hashSizeAttrName)
}
func (fs *xattrFs) SetHashSize(path string, h api.Hash, s int64) error {
	return fs.writeHashSize(path, hashSizeAttrName, h, s)
}
func (fs *xattrFs) ARISize(path string) (string, int64, error) {
	return fs.readARISize(path, ariSizeAttrName)
}
func (fs *xattrFs) SetARISize(path, ari string, s int64) error {
	return fs.writeARISize(path, ariSizeAttrName, ari, s)
}
func (fs *xattrFs) HashStat(path string) (*api.HashStat, error) {
	return fs.readHashStat(path, hashStatAttrName)
}
func (fs *xattrFs) DeepTree(path string) (api.Hash, error) {
	return fs.readHash(path, deepTreeAttrName)
}
func (fs *xattrFs) SetDeepTree(path string, h api.Hash) error {
	return fs.writeHash(path, deepTreeAttrName, h)
}

func (fs *xattrFs) Debug(path string) (bool, error) {
	return fs.readBool(path, debugAttrName)
}
func (fs *xattrFs) SetDebug(path string, val bool) error {
	return fs.writeBool(path, debugAttrName, val)
}
func (fs *xattrFs) CreateControl(path, directory string) error {
	return fs.writeString(path, ctrlAttrName, fmt.Sprintf("ctrl:%s", directory))
}
func (fs *xattrFs) CreateRamfs(path, directory string) error {
	return fs.writeString(path, ctrlAttrName, fmt.Sprintf("mem:%s", directory))
}
func (fs *xattrFs) CreateGitObjects(path, directory string) error {
	return fs.writeString(path, ctrlAttrName, fmt.Sprintf("gitobj:%s", directory))
}

func (fs *xattrFs) CreateGit(path, directory, head string) error {
	gitPath := filepath.Join(path, directory)

	if err := os.MkdirAll(gitPath, 0755); err != nil {
		return fmt.Errorf("failed to create base git directory: %w", err)
	}
	// Mark git as excluded from parent
	if err := fs.SetExcluded(gitPath, true); err != nil {
		return fmt.Errorf("failed to set git to excluded: %w", err)
	}
	if err := fs.CreateGitObjects(gitPath, "objects"); err != nil {
		return fmt.Errorf("failed to create intercepted objects dir: %w", err)
	}

	dirs := []string{
		filepath.Join(gitPath, "objects", "info"),
		filepath.Join(gitPath, "objects", "pack"),
		filepath.Join(gitPath, "refs"),
		filepath.Join(gitPath, "refs", "heads"),
		filepath.Join(gitPath, "refs", "tags"),
	}

	for _, d := range dirs {
		if err := os.MkdirAll(d, 0755); err != nil {
			return fmt.Errorf("failed to natively create directory %s: %w", d, err)
		}
	}

	headFile := filepath.Join(gitPath, "HEAD")
	if err := os.WriteFile(headFile, []byte(head), 0644); err != nil {
		return fmt.Errorf("failed to write HEAD file: %w", err)
	}

	configFile := filepath.Join(gitPath, "config")
	if err := os.WriteFile(configFile, []byte(gitConfigString), 0644); err != nil {
		return fmt.Errorf("failed to write config file: %w", err)
	}

	if err := fs.CreateGitIndex(path, directory, path, head); err != nil {
		return fmt.Errorf("failed to build git index during project creation: %w", err)
	}

	return nil
}

func (fs *xattrFs) CreateGitIndex(path, directory, worktree, hash string) error {
	absPath, err := filepath.Abs(path)
	if err != nil {
		return err
	}

	var absWorkTree string
	if worktree == "" {
		absWorkTree = absPath
	} else {
		absWorkTree, err = filepath.Abs(worktree)
		if err != nil {
			return err
		}
	}
	if info, err := os.Stat(absWorkTree); err != nil {
		if os.IsNotExist(err) {
			return fmt.Errorf("fatal: working tree '%s' does not exist", absWorkTree)
		}
		return fmt.Errorf("failed to stat working tree: %w", err)
	} else if !info.IsDir() {
		return fmt.Errorf("fatal: working tree '%s' is not a directory", absWorkTree)
	}
	gitDir := filepath.Join(absPath, directory)
	if err := os.MkdirAll(gitDir, 0755); err != nil {
		return fmt.Errorf("failed to create base git directory: %w", err)
	}

	headFile := filepath.Join(gitDir, "HEAD")
	if _, err := os.Stat(headFile); os.IsNotExist(err) {
		if err := os.WriteFile(headFile, []byte(hash+"\n"), 0644); err != nil {
			return fmt.Errorf("failed to write HEAD file: %w", err)
		}
	}

	indexPath := filepath.Join(gitDir, "index")
	if err := syscall.Mknod(indexPath, syscall.S_IFREG|uint32(0644), 0); err != nil {
		return fmt.Errorf("failed to create index file '%s' : %v", indexPath, err)
	}

	realWorkTree, err := filepath.EvalSymlinks(absWorkTree)
	if err != nil {
		realWorkTree = absWorkTree
	}
	scanResult, err := gitindex.Scan(realWorkTree, hash, 64*1024)
	if err != nil {
		return fmt.Errorf("failed to scan worktree: %w", err)
	}

	if err := gitindex.Write(indexPath, scanResult); err != nil {
		return fmt.Errorf("failed to create git index: %w", err)
	}

	now := time.Now().Add(1 * time.Second)
	if err := os.Chtimes(indexPath, now, now); err != nil {
		return fmt.Errorf("failed to bump index mtime: %w", err)
	}
	return nil

}

func (fs *xattrFs) Tap(cmd *exec.Cmd) (*api.TapCmdResult, error) {
	return RunTapCmd(cmd, fs.hasher, fs.tapPath, nil)
}
func (fs *xattrFs) TapResolved(cmd *exec.Cmd) (*api.TapCmdResolvedResult, error) {
	return RunTapCmdResolved(cmd, fs, fs.hasher, fs.tapPath, nil)
}
func (fs *xattrFs) Status() (*api.Status, error) {
	ready, err := fs.readBool(fs.mountPath, fsStatusAttrName)
	if err != nil {
		return nil, err
	}
	return &api.Status{Ready: ready}, nil
}
func (fs *xattrFs) BackendStat(path string) (*api.BackendStat, error) {
	return fs.readBackendStat(path, backendStatAttrName)
}
