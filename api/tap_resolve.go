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
	"encoding/json"
	"io/fs"
	"path/filepath"
)

type TapCmdResolvedResult struct {
	Reads   map[string]HashStat
	Writes  map[string]HashStat
	Deletes map[string]struct{}
	TapInfo
}

func NewTapCmdResolvedResult() *TapCmdResolvedResult {
	return &TapCmdResolvedResult{
		Reads:   make(map[string]HashStat),
		Writes:  make(map[string]HashStat),
		Deletes: make(map[string]struct{}),
	}

}
func (t *TapCmdResolvedResult) UnmarshalJSON(data []byte, hasher Hasher) error {
	type hashStat struct {
		Hash string
		Mode fs.FileMode
		Size int64
	}
	type tapCmdResolvedResult struct {
		Reads   map[string]hashStat
		Writes  map[string]hashStat
		Deletes map[string]struct{}
		TapInfo
	}
	var tmp = tapCmdResolvedResult{
		Reads:   make(map[string]hashStat),
		Writes:  make(map[string]hashStat),
		Deletes: make(map[string]struct{}),
	}
	if err := json.Unmarshal(data, &tmp); err != nil {
		return err
	}
	for k, v := range tmp.Reads {
		h, err := hasher.NewHash(v.Hash)
		if err != nil {
			return err
		}
		t.Reads[k] = HashStat{Hash: h,
			Mode: v.Mode,
			Size: v.Size}
	}
	for k, v := range tmp.Writes {
		h, err := hasher.NewHash(v.Hash)
		if err != nil {
			return err
		}
		t.Writes[k] = HashStat{Hash: h,
			Mode: v.Mode,
			Size: v.Size}
	}
	t.Deletes = tmp.Deletes
	return nil

}

// When running a tap resolve we need to track the file transactions and build
// a corresponding tree that represents the state of the writes so that at the
// end of the build we can iterate over and read the HashStat.  This is needed
// for the rename directory case.
func splitPath(p string) []string {
	p = filepath.Clean(p)
	sep := string(filepath.Separator)
	var components []string

	for p != "." && p != "" && p != sep {
		p = filepath.Clean(p)
		dir, file := filepath.Split(p)

		if file == "" {
			if dir != "" {
				components = append(components, filepath.Clean(dir))
			}
			break
		}
		components = append([]string{file}, components...)
		p = dir
	}

	return components
}

const defDirMode fs.FileMode = fs.FileMode(fs.ModeDir | 0777)

type TapTrackNode struct {
	Name     string
	Mode     fs.FileMode
	IsAuto   bool
	Children map[string]*TapTrackNode
}
type WalkFunc = func(path string, mode fs.FileMode) error

func (t *TapTrackNode) Walk(parentPath string, fn WalkFunc) error {
	ourPath := filepath.Join(parentPath, t.Name)
	if !t.IsAuto {
		if err := fn(ourPath, t.Mode); err != nil {
			return err
		}
	}

	for _, c := range t.Children {
		if err := c.Walk(ourPath, fn); err != nil {
			return err
		}
	}
	return nil
}

type TapTrack struct {
	Root TapTrackNode
}

func NewTapTrackNode(name string, mode fs.FileMode, isAuto bool) *TapTrackNode {
	return &TapTrackNode{
		Name:     name,
		Mode:     mode,
		IsAuto:   isAuto,
		Children: make(map[string]*TapTrackNode),
	}
}
func NewTapTrack() *TapTrack {
	return &TapTrack{
		Root: *NewTapTrackNode("/", defDirMode, true),
	}
}
func (t *TapTrack) FindParent(path string) (*TapTrackNode, string) {
	comps := splitPath(path)
	if len(comps) == 0 {
		return nil, ""
	}

	name := comps[len(comps)-1]
	comps = comps[:len(comps)-1]
	var dir *TapTrackNode = &t.Root

	for _, dirName := range comps {
		if cd, exists := dir.Children[dirName]; exists {
			dir = cd
		} else {
			// Auto create parents as needed
			newDir := NewTapTrackNode(dirName, defDirMode, true)
			dir.Children[dirName] = newDir
			dir = newDir
		}
	}
	return dir, name
}
func (t *TapTrack) Add(path string, mode fs.FileMode) {
	dir, name := t.FindParent(path)
	dir.Children[name] = NewTapTrackNode(name, mode, false)
}
func (t *TapTrack) Remove(path string) {
	dir, name := t.FindParent(path)
	delete(dir.Children, name)
}
func (t *TapTrack) Rename(path, newPath string, newMode fs.FileMode) {
	dir, name := t.FindParent(path)
	newDir, newName := t.FindParent(newPath)

	if oldNode, exists := dir.Children[name]; !exists {
		newDir.Children[newName] = NewTapTrackNode(newName, newMode, false)
	} else {
		oldNode.Mode = newMode
		oldNode.Name = newName
		newDir.Children[newName] = oldNode
		delete(dir.Children, name)
	}
}

func (t *TapTrack) Walk(fn WalkFunc) error {
	return t.Root.Walk("", fn)
}
