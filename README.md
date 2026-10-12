CASFS - Content-Addressable Storage File-System
===

CASFS is a Linux file system in which files and directories are identified by
the hash of their content, using the Git object model (SHA-1). Trees are
loaded lazily: a directory or file is materialized from its hash only when it
is accessed, and missing objects are fetched on demand from a user-space cache
daemon over a Unix domain socket. CASFS also provides "tapping", which records
the file-system activity of a process group, for precise build dependency
tracking.

CASFS is the file-system layer of ABFS (Another Build File Service).

> [!NOTE]
> This is not an officially supported Google product.

## License

This repository contains code under two licenses.

* The Linux kernel module and everything else outside `api/` (the C
  sources, `Kconfig`, `Makefile`, `ctrl/`, build scripts and tests) are
  licensed under the GNU General Public License, version 2 only (SPDX
  `GPL-2.0`); see [LICENSE](LICENSE).
* `ctrl/inode.c` is adapted from the Linux kernel's `fs/ramfs/inode.c`
  (Copyright (C) 2000 Linus Torvalds, 2000 Transmeta Corp.) and keeps its
  upstream notice.
* The Go user-space API and CLI under [`api/`](api/) are licensed under the
  Apache License, Version 2.0; see [api/LICENSE](api/LICENSE).

Code under `third_party/` comes from other projects; see the `LICENSE`
and `METADATA` files in each of its directories. The
`SPDX-License-Identifier` or license header in each file is authoritative.

## Repository layout

| Path | Contents |
| :--- | :--- |
| `*.c`, `*.h`, `Kconfig`, `Makefile` | The `casfs` kernel module. |
| `ctrl/` | Control interface (tapping, Git object virtualization). |
| `api/` | Go library and `casfsapi` CLI for the extended-attribute API, plus Go tests. |
| `docs/` | Design notes for the cache, file and provider subsystems. |
| `tests/` | Shell test runners (require root and a loaded module). |
| `third_party/linux/` | `checkpatch.pl` and its data files, and `.clang-format`, from the Linux kernel (see its `METADATA`). |

## Building

Building the module requires the headers for the running kernel
(`/lib/modules/$(uname -r)/build`). The Go code requires a recent Go
toolchain (see `api/go.mod`).

```shell
make ko        # build casfs.ko for the running kernel
make api       # build the api/cmd/casfsapi CLI
make apitests  # build the api/tests test binary
```

`make` with no target builds all of the above and installs the git hooks
from `hooks/`.

The module version comes from `git describe`, or from the `VERSION` file
when no release tag is available.

### Installing with DKMS

A module loaded with `insmod` is not loaded again after a reboot, and it
stops loading once the kernel is updated (`disagrees about version of
symbol ...`). To avoid both, install it with
[DKMS](https://github.com/dkms-project/dkms), which rebuilds it whenever a new
kernel and its headers are installed:

```shell
sudo apt-get install --no-install-recommends dkms   # Debian/Ubuntu
make dkms-install
```

Run `make dkms-install` as your normal user; it uses `sudo` where needed.
It copies the sources to `/usr/src/casfs-<version>`, replaces any other
casfs version registered with DKMS, builds and installs the module for every
kernel with headers installed, loads it if no casfs file system is mounted,
and adds `/etc/modules-load.d/casfs.conf` so that it is loaded at boot.
`sudo modprobe casfs` and `sudo modprobe -r casfs` then work as for any
other module. Re-run it after updating the sources. `make dkms-uninstall`
removes it again.

To get the module for future kernels, keep the headers for them installed,
for example with the headers metapackage that matches your kernel
metapackage (`linux-headers-gcp` for `linux-image-gcp`,
`linux-headers-cloud-amd64` for `linux-image-cloud-amd64`, and so on).

## Quick start

```shell
make ko
sudo insmod casfs.ko

CACHE_DIR=~/.cache/cas
MNT_DIR=/mnt
mkdir -p $CACHE_DIR
sudo mount -t casfs none $MNT_DIR -o cache=$CACHE_DIR -o local
```

### Tests

After compiling and inserting the kernel module, run `tests/run_tests.sh` to
confirm basic functionality is available. The tests mount CASFS and use
`sudo`, so run them on a test machine.

### Control Interface
The CASFS contains a control interface allowing users to interface with the file system.  In order
to use the control interface the user needs to identify a directory to attach the control interface.
This is done via the `casfs.ctrl` attribute of the directory, the value used defines the directory
to create under the input directory and the control interface tree will be populated.

```shell
setfattr -n casfs.ctrl /mnt/casfs/chroot -v .ctrl

tree /mnt/casfs/chroot/.ctrl/
/mnt/casfs/chroot/.ctrl/
└── tap

```
#### Tap
The tap control interface allows a process to record all file-system activity associated with a
program session.  This is accomplished by opening a temporary file off the tap control directory,
once the file is open any file-system access of the CASFS will be written to the unnamed temporary
file associated with the program session until the user ends the tap by lseek-ing to `SEEK_HOLE`.
The user can then read the results by lseek-ing back to the start of the file and reading the data.

```c
setpgid(0,0);
fd = open(".ctrl/tap", O_TMPFILE | O_RDWR | O_EXCL, S_IRUSR | S_IWUSR);

// Run programs...

// End session
end = lseek(fd, 0, SEEK_HOLE);
(void)lseek(fd, 0, SEEK_SET);

// Read data
read(fd, buf, end);
```

For a complete example, see the `casfsapi tap` command in
[`api/cmd/tap`](api/cmd/tap) and the Go implementation in
[`api/xattr/tap.go`](api/xattr/tap.go).

## Documentation

* [docs/cache.md](docs/cache.md): local cache and scratch storage.
* [docs/file.md](docs/file.md): file and blob operations.
* [docs/provider.md](docs/provider.md): the kernel/user-space provider protocol.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). New files must carry the license
header that matches their location (see [License](#license) above). Kernel C code
follows the Linux kernel coding style: `make install-hooks` installs git
hooks that run `clang-format` and `third_party/linux/scripts/checkpatch.pl`.
Go code must be `gofmt`-clean and pass `go vet`.

## Security

Eligibility for the [Google Open Source Software Vulnerability Rewards
Program](https://bughunters.google.com/open-source-security) is determined by
the [Google Open Source Software Vulnerability Reward Program
Rules](https://bughunters.google.com/about/rules/open-source/google-open-source-software-vulnerability-reward-program-rules).
