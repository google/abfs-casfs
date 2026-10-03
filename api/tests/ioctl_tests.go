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

package tests

import (
	"testing"
	"unsafe"

	"github.com/google/abfs-casfs/api/ioctl"
)

func TestCasfsDirEntryHeaderLayout(t *testing.T) {
	var h ioctl.CasfsDirEntryHeader

	// Check total size (108 bytes of fields + 4 bytes struct 8-byte alignment padding = 112)
	expectedSize := uintptr(112)
	if sz := unsafe.Sizeof(h); sz != expectedSize {
		t.Fatalf("sizeof(CasfsDirEntryHeader) = %d; want %d", sz, expectedSize)
	}

	// Check field offsets
	tests := []struct {
		name   string
		got    uintptr
		expect uintptr
	}{
		{"CtimeSec", unsafe.Offsetof(h.CtimeSec), 0},
		{"CtimeNsec", unsafe.Offsetof(h.CtimeNsec), 4},
		{"MtimeSec", unsafe.Offsetof(h.MtimeSec), 8},
		{"MtimeNsec", unsafe.Offsetof(h.MtimeNsec), 12},
		{"Size", unsafe.Offsetof(h.Size), 16},
		{"Ino", unsafe.Offsetof(h.Ino), 24},
		{"Dev", unsafe.Offsetof(h.Dev), 32},
		{"Mode", unsafe.Offsetof(h.Mode), 36},
		{"Uid", unsafe.Offsetof(h.Uid), 40},
		{"Gid", unsafe.Offsetof(h.Gid), 44},
		{"Dirtiness", unsafe.Offsetof(h.Dirtiness), 48},
		{"Type", unsafe.Offsetof(h.Type), 52},
		{"BackendType", unsafe.Offsetof(h.BackendType), 53},
		{"Flags", unsafe.Offsetof(h.Flags), 54},
		{"NameLen", unsafe.Offsetof(h.NameLen), 56},
		{"AriLen", unsafe.Offsetof(h.AriLen), 58},
		{"Sha", unsafe.Offsetof(h.Sha), 60},
		{"CommitSha", unsafe.Offsetof(h.CommitSha), 80},
		{"Pad", unsafe.Offsetof(h.Pad), 100},
	}

	for _, tc := range tests {
		if tc.got != tc.expect {
			t.Errorf("Offset of %s = %d; want %d", tc.name, tc.got, tc.expect)
		}
	}
}
