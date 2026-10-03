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
	"io"
	"os"
	"syscall"

	"github.com/google/abfs-casfs/api"
)

// Blob related tests

type blobTest struct {
	fs     api.FS
	hasher api.Hasher
}

func (b *blobTest) Create(path string) (*os.File, error) {
	return os.Create(path)
}

func (b *blobTest) Read(path string) (*os.File, error) {
	file, err := os.OpenFile(path, os.O_RDONLY, DefaultFileMode)
	if err != nil {
		return nil, err
	}
	_, err = io.ReadAll(file)
	return file, err

}

func (b *blobTest) Write(path, content string) (*os.File, error) {
	mode := DefaultFileMode
	file, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY, mode)
	if err != nil {
		return nil, err
	}
	if _, err := file.WriteString(content); err != nil {
		return nil, err
	}
	return file, nil
}
func (b *blobTest) Snapshot(path string) error {
	_, err := b.fs.Hash(path)
	return err
}
func (b *blobTest) CreateViaRef(path, refPath, data string) error {
	f, err := b.Write(refPath, data)
	if err != nil {
		return err
	}
	f.Close()
	hash, err := b.fs.Hash(refPath)
	if err != nil {
		return err
	}
	osMode := 0644
	if err := syscall.Mknod(path, syscall.S_IFREG|uint32(osMode), 0); err != nil {
		return err
	}
	if err := b.fs.SetHash(path, hash); err != nil {
		return err
	}
	return nil
}
func (b *blobTest) LoadOnly(path string) error {
	// To get into the loaded state we want to open the file but not write to it
	file, err := b.Create(path)
	if err != nil {
		return err
	}
	file.Close()
	file, err = os.OpenFile(path, os.O_RDONLY, DefaultFileMode)
	if err != nil {
		return err
	}
	file.Close()
	return nil
}
func (b *blobTest) ReadOnly(path, refPath string, openTruncate, truncate bool) error {
	// To get into the loaded state we want to open the file and read it
	// to do this we must have data will set this up with ref path
	if err := b.CreateViaRef(path, refPath, "test data content"); err != nil {
		return err
	}
	f, err := b.Read(path)
	if err != nil {
		return err
	}
	if openTruncate {
		if err := os.Truncate(path, 0); err != nil {
			return err
		}

	}
	f.Close()

	if truncate {
		if err := os.Truncate(path, 0); err != nil {
			return err
		}
	}

	return nil
}
func (b *blobTest) WriteOnly(path, refPath string, snapshot bool) error {
	// To get into the loaded state we want to open the file and write to it
	// to do this we must have data will set this up with ref path
	if err := b.CreateViaRef(path, refPath, "test data content"); err != nil {
		return err
	}
	f, err := b.Write(path, "New content")
	if err != nil {
		return err
	}
	f.Close()
	if snapshot {
		if _, err := b.fs.Hash(path); err != nil {
			return err
		}
	}
	return nil
}

func (b *blobTest) ReadWrite(path, refPath string, snapshot, openTruncate, truncate bool) error {
	if err := b.CreateViaRef(path, refPath, "test data content"); err != nil {
		return err
	}
	fR, err := b.Read(path)
	if err != nil {
		return err
	}
	defer fR.Close()
	f, err := b.Write(path, "New content")
	if err != nil {
		return err
	}
	if openTruncate {
		if err := os.Truncate(path, 0); err != nil {
			return err
		}
	}
	f.Close()
	if snapshot {
		if _, err := b.fs.Hash(path); err != nil {
			return err
		}
	}
	if truncate {
		if err := os.Truncate(path, 0); err != nil {
			return err
		}
	}
	return nil
}

func (b *blobTest) WriteRead(path, refPath string, snapshot, openTruncate, truncate bool) error {
	if err := b.CreateViaRef(path, refPath, "test data content"); err != nil {
		return err
	}
	f, err := b.Write(path, "New content")
	if err != nil {
		return err
	}
	defer f.Close()
	if openTruncate {
		if err := os.Truncate(path, 0); err != nil {
			return err
		}
	}
	fR, err := b.Read(path)
	if err != nil {
		return err
	}
	defer fR.Close()

	if snapshot {
		if _, err := b.fs.Hash(path); err != nil {
			return err
		}
	}
	if truncate {
		if err := os.Truncate(path, 0); err != nil {
			return err
		}
	}
	return nil
}
