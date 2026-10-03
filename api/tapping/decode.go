// Copyright 2026 Google LLC
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

package tapping

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"io"

	"github.com/google/abfs-casfs/api"
)

const TapSeqVersion = 0x03
const MaxChunkSize = 8192

func TapDecode(reader io.Reader, hasher api.Hasher, taps chan *api.TapFSEntry) error {
	buf := bufio.NewReaderSize(reader, MaxChunkSize)

	v, err := buf.ReadByte()
	if err != nil {
		if err == io.EOF {
			return nil
		}
		return err
	}
	if v != TapSeqVersion {
		return fmt.Errorf("unsupported tap version %d", v)
	}

	for {
		t, err := buf.ReadByte()
		if err != nil {
			if err == io.EOF {
				return nil
			}
			return err
		}
		dirty, err := buf.ReadByte()
		if err != nil {
			return err
		}
		isARI, err := buf.ReadByte()
		if err != nil {
			return err
		}
		tap := new(api.TapFSEntry)
		tap.Type = api.TapType(t)
		tap.Dirty = (dirty != 0)
		tap.Hash = hasher.ZeroHash()
		tap.IsARI = (isARI != 0)

		if tap.IsARI {
			tap.ARI, err = buf.ReadString('\x00')
			if err != nil {
				return err
			}
			tap.ARI = tap.ARI[:len(tap.ARI)-1]
		} else {
			if _, err := tap.Hash.Read(buf); err != nil {
				return err
			}
		}
		if err := binary.Read(buf, binary.NativeEndian, &tap.Size); err != nil {
			return err
		}

		var mode uint32
		if err := binary.Read(buf, binary.NativeEndian, &mode); err != nil {
			return err
		}
		tap.Mode = api.OSModeToFSMode(mode)

		tap.Path, err = buf.ReadString('\x00')
		if err != nil {
			return err
		}
		tap.Path = tap.Path[:len(tap.Path)-1]

		tap.AuxPath, err = buf.ReadString('\x00')
		if err != nil {
			return err
		}
		tap.AuxPath = tap.AuxPath[:len(tap.AuxPath)-1]
		taps <- tap
	}
}
