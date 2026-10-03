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

package main

import (
	"log"

	"go.coder.com/cli"

	"github.com/google/abfs-casfs/api/cmd/arisize"
	"github.com/google/abfs-casfs/api/cmd/binhash"
	"github.com/google/abfs-casfs/api/cmd/control"
	"github.com/google/abfs-casfs/api/cmd/create"
	"github.com/google/abfs-casfs/api/cmd/debug"
	"github.com/google/abfs-casfs/api/cmd/deeptree"
	"github.com/google/abfs-casfs/api/cmd/dirlist"
	"github.com/google/abfs-casfs/api/cmd/dirscan"
	"github.com/google/abfs-casfs/api/cmd/dirty"
	"github.com/google/abfs-casfs/api/cmd/excluded"
	"github.com/google/abfs-casfs/api/cmd/find"
	"github.com/google/abfs-casfs/api/cmd/git"
	"github.com/google/abfs-casfs/api/cmd/gitobj"
	"github.com/google/abfs-casfs/api/cmd/hash"
	"github.com/google/abfs-casfs/api/cmd/hashsize"
	"github.com/google/abfs-casfs/api/cmd/loaded"
	"github.com/google/abfs-casfs/api/cmd/ls"
	"github.com/google/abfs-casfs/api/cmd/root"
	"github.com/google/abfs-casfs/api/cmd/stat"
	"github.com/google/abfs-casfs/api/cmd/status"
	"github.com/google/abfs-casfs/api/cmd/tap"
)

func main() {
	log.SetFlags(0)
	r := root.NewRootCommand()
	r.Commands = []cli.Command{
		arisize.NewCmd(r),
		binhash.NewCmd(r),
		control.NewCmd(r),
		create.NewCmd(r),
		debug.NewCmd(r),
		deeptree.NewCmd(r),
		dirlist.NewCmd(r),
		dirscan.NewCmd(r),
		dirty.NewCmd(r),
		excluded.NewCmd(r),
		find.NewCmd(r),
		hash.NewCmd(r),
		hashsize.NewCmd(r),
		loaded.NewCmd(r),
		ls.NewCmd(r),
		tap.NewCmd(r),
		git.NewCmd(r),
		gitobj.NewCmd(r),
		stat.NewCmd(r),
		status.NewCmd(r),
	}

	cli.RunRoot(r)
}
