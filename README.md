CASFS - Content-Addressable Storage File-System
===

CASFS is a Linux file system in which files and directories are identified by
the hash of their content, using the Git object model (SHA-1). Everything is
loaded lazily: a directory or file is materialized from its hash only when it
is accessed, and missing objects are fetched on demand from a user-space cache
daemon over a Unix domain socket. CASFS also provides "tapping", which records
the file-system activity of a process group, for precise build dependency
tracking.

CASFS is the file-system layer of [ABFS (Another Build File Service)](https://github.com/google/abfs).

> [!NOTE]
> This is not an officially supported Google product.

## Compatibility

We have an upcoming release planned that will fix many compatibility issues for a wider range of kernels. For the current initial release, we have this AI-scripted analysis of compatibility:

### Kernel compatibility

CASFS is built out of tree against the headers of the target kernel. The
results below come from compile-and-link checks on x86_64 against the
[Ubuntu mainline kernel](https://kernel.ubuntu.com/mainline/) header packages
(the newest point release of each series as of October 2026). The module was
not loaded or tested on these kernels; a successful build is not a guarantee
of runtime correctness.

Builds successfully:

- 5.15 (5.15.223)
- 6.1 (6.1.190)
- 6.8 (6.8.12)
- 6.9 (6.9.12)
- 6.10 (6.10.14)
- 6.11 (6.11.11)
- 6.12 (6.12.50, 6.12.113)
- 6.13 (6.13.12)
- 6.15 (6.15.11)
- 6.16 (6.16.12)
- 6.17 (6.17.12)
- 6.18 (6.18.57)
- 7.0 (7.0.1, 7.0.14)
- 7.1 (7.1.13)
- 7.2 (7.2.6)

Distribution kernels that have also been built and used with ABFS:

- Debian 13: 6.12.107+deb13-cloud-amd64
- Ubuntu 26.04 on Google Compute Engine: 7.0.0-1011-gcp, 7.0.0-1014-gcp

Known build failures:

- 5.10 and older: `vfs_unlink()` signature mismatch in `cache.c`.
- 6.6 (6.6.159): uses `inode->i_ctime`, which was replaced by the ctime
  accessors in 6.6.
- 6.7 (6.7.10): `<linux/backing-file.h>` is missing (it first appeared in
  6.8).
- 6.14 (6.14.11): `d_revalidate` signature mismatch in `dir.c`.
- 6.19 (6.19.14): `vfs_mkdir()` / `vfs_rmdir()` signature mismatch in
  `compat_dir.h`.
- 7.3-rc3: `create` inode operation signature mismatch in `dir.c`.

Not yet tested: 6.2 through 6.5.

## Use with ABFS

Most people should see the [ABFS documentation](https://github.com/google/abfs) for using CASFS within the context of ABFS instead of this documentation directly.

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
header that matches their location (see [License](#license) below). Kernel C code
follows the Linux kernel coding style: `make install-hooks` installs git
hooks that run `clang-format` and `third_party/linux/scripts/checkpatch.pl`.
Go code must be `gofmt`-clean and pass `go vet`.

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

## Security

Eligibility for the [Google Open Source Software Vulnerability Rewards
Program](https://bughunters.google.com/open-source-security) is determined by
the [Google Open Source Software Vulnerability Reward Program
Rules](https://bughunters.google.com/about/rules/open-source/google-open-source-software-vulnerability-reward-program-rules).
