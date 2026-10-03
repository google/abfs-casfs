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
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/fsnotify/fsnotify"
)

var done = make(chan struct{})

func watchDir(watcher *fsnotify.Watcher, path string) error {
	return filepath.Walk(path, func(subPath string, info os.FileInfo, err error) error {
		if err != nil {
			if os.IsNotExist(err) {
				return nil
			}
			return err
		}
		if info.IsDir() {
			if err := watcher.Add(subPath); err != nil {
				if os.IsNotExist(err) {
					return nil
				}
				return err
			}
		}
		return nil
	})
}

func Watch(objectDir, blobDir, scratchDir string) error {
	watcher, err := fsnotify.NewWatcher()
	if err != nil {
		return err
	}
	defer watcher.Close()

	if err := watchDir(watcher, blobDir); err != nil {
		return err
	}
	if err := watchDir(watcher, scratchDir); err != nil {
		return err
	}

	type FileType int
	const (
		BlobFile FileType = iota
		ScratchFile
		Unknown
	)
	getPathType := func(p string) FileType {
		if strings.HasPrefix(p, blobDir) {
			return BlobFile
		}
		if strings.HasPrefix(p, scratchDir) {
			return ScratchFile
		}
		return Unknown
	}

	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	verbose := false

	for {
		select {
		case <-done:
			return err
		case event, ok := <-watcher.Events:
			if !ok {
				continue
			}
			if event.Op&fsnotify.Write == fsnotify.Write && event.Op&fsnotify.Create != fsnotify.Create {
				t := getPathType(event.Name)
				if t == BlobFile {
					log.Printf("Invalid blob write to %s", event.Name)
					err = fmt.Errorf("detected write to blob path")
				}
			}
			if verbose {
				log.Println(event.String())
			}
		case wErr, ok := <-watcher.Errors:
			if !ok {
				continue
			}
			log.Printf("watch: %s", wErr)
		case <-ticker.C:
		}
	}
}

func EndWatch() {
	close(done) // Closing the channel broadcasts the stop signal to the select block
}
