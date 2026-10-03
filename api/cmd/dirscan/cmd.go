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

package dirscan

import (
	"encoding/hex"
	"fmt"
	"log"
	"os"
	"syscall"
	"unsafe"

	"github.com/spf13/pflag"
	"go.coder.com/cli"

	"github.com/google/abfs-casfs/api/cmd/args"
	"github.com/google/abfs-casfs/api/cmd/root"
	"github.com/google/abfs-casfs/api/ioctl"
)

type cmd struct {
	rootCmd *root.RootCommand
	bufSize int
}

func NewCmd(rootCmd *root.RootCommand) *cmd {
	return &cmd{
		rootCmd: rootCmd,
		bufSize: 64 * 1024,
	}
}

func (c *cmd) RegisterFlags(fl *pflag.FlagSet) {
	fl.IntVarP(&c.bufSize, "buf-size", "b", 64*1024, "User-space buffer size for the ioctl batch in bytes")
}

func (c *cmd) Spec() cli.CommandSpec {
	return cli.CommandSpec{
		Name:  "dirscan",
		Usage: "[-b BUF_SIZE] [DIR]",
		Desc:  "Read and dump CASFS inode state and hashes via CASFS_IOC_READ_DIR_ENTRIES ioctl.",
	}
}

func (c *cmd) Run(fl *pflag.FlagSet) {
	path := args.EnsureSingleArg(fl)

	f, err := os.Open(path)
	if err != nil {
		log.Fatalf("failed to open directory '%s': %v", path, err)
	}
	defer f.Close()

	buf := make([]byte, c.bufSize)
	var cookie uint64

	fmt.Printf("%-8s %-7s %-10s %-10s %-5s %-6s %-40s %s\n",
		"TYPE", "MODE", "SIZE", "INODE", "DIRTY", "FLAGS", "SHA-1", "NAME (ARI)")
	fmt.Println("-------------------------------------------------------------------------------------------------------------------")

	for {
		req := ioctl.CasfsReadDirArgs{
			BufPtr:  uint64(uintptr(unsafe.Pointer(&buf[0]))),
			BufSize: uint64(len(buf)),
			Cookie:  cookie,
		}

		_, _, errno := syscall.Syscall(
			syscall.SYS_IOCTL,
			f.Fd(),
			ioctl.CasfsIocReadDirEntries(),
			uintptr(unsafe.Pointer(&req)),
		)
		if errno != 0 {
			log.Fatalf("ioctl failed: %v", errno)
		}

		var offset uint64
		for offset < req.BytesWritten {
			if offset+uint64(ioctl.SizeOfCasfsDirEntryHeader) > req.BytesWritten {
				log.Fatalf("corrupted entry at offset %d", offset)
			}

			hdr := (*ioctl.CasfsDirEntryHeader)(unsafe.Pointer(&buf[offset]))
			totalRecLen := uint64(ioctl.SizeOfCasfsDirEntryHeader) + uint64(hdr.NameLen) + 1 + uint64(hdr.AriLen) + 1
			if offset+totalRecLen > req.BytesWritten {
				log.Fatalf("record length overflows buffer: offset %d, totalRecLen %d, bytesWritten %d",
					offset, totalRecLen, req.BytesWritten)
			}

			nameStart := offset + uint64(ioctl.SizeOfCasfsDirEntryHeader)
			nameEnd := nameStart + uint64(hdr.NameLen)

			name := string(buf[nameStart:nameEnd])
			ariStart := nameEnd + 1
			ariEnd := ariStart + uint64(hdr.AriLen)

			ari := string(buf[ariStart:ariEnd])
			offset = ariEnd + 1

			shaHex := hex.EncodeToString(hdr.Sha[:])

			var flagStr string
			if hdr.Flags&ioctl.CASFS_FLAG_LOADED != 0 {
				flagStr += "L"
			}
			if hdr.Flags&ioctl.CASFS_FLAG_EXCLUDED != 0 {
				flagStr += "E"
			}
			if hdr.Flags&ioctl.CASFS_FLAG_HAS_ARI != 0 {
				flagStr += "A"
			}
			if flagStr == "" {
				flagStr = "-"
			}

			displayName := name
			if ari != "" {
				// Display full or short ARI next to the name
				displayName = fmt.Sprintf("%s [ari:%s]", name, ari)
			}

			fmt.Printf("%-8s %06o  %-10d %-10d %-5d %-6s %-40s %s\n",
				ioctl.TypeString(hdr.Type),
				hdr.Mode,
				hdr.Size,
				hdr.Ino,
				hdr.Dirtiness,
				flagStr,
				shaHex,
				displayName,
			)
		}

		cookie = req.Cookie
		if req.HasMore == 0 {
			break
		}
	}
}
