#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Google LLC
#
# Installs or removes the casfs kernel module with DKMS. With DKMS the module
# is rebuilt automatically whenever a new kernel (with headers) is installed,
# `modprobe casfs` works, and the module is loaded at boot through
# /etc/modules-load.d/casfs.conf.
#
# Usage:
#   scripts/dkms.sh install <version>
#   scripts/dkms.sh uninstall
#
# Normally run through `make dkms-install` / `make dkms-uninstall`, which
# generate version.h and pass the module version. Run it as your normal user
# (it uses sudo where needed) so that version.h and git-version in the source
# tree are not owned by root.
#
# `install` copies the module sources to /usr/src/casfs-<version>, removes
# any other casfs version registered with DKMS, builds and installs the module
# for every kernel that has headers installed, and (re)loads it if no casfs
# file system is mounted. `uninstall` unloads the module if possible and
# removes everything `install` added.

set -euo pipefail

readonly NAME="casfs"
readonly SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly DKMS_TREE="/var/lib/dkms"
readonly MODULES_LOAD_CONF="/etc/modules-load.d/${NAME}.conf"

# dkms, modinfo and rmmod live in /usr/sbin, which is not on a normal user's
# PATH on Debian.
export PATH="${PATH}:/usr/sbin:/sbin"

SUDO="sudo"
if [[ "$(id -u)" -eq 0 ]]; then
	SUDO=""
fi

die() {
	echo "ERROR: $*" >&2
	exit 1
}

warn() {
	echo "WARNING: $*" >&2
}

usage() {
	sed -n '/^# Usage:/,/^#$/p' "$0" | sed 's/^# \{0,1\}//' >&2
	exit 1
}

# Prints the casfs versions registered with DKMS.
registered_versions() {
	[[ -d "${DKMS_TREE}/${NAME}" ]] || return 0
	# Version directories; skips the kernel-<release>-<arch> symlinks.
	find "${DKMS_TREE}/${NAME}" -mindepth 1 -maxdepth 1 -type d \
		! -name original_module -printf '%f\n'
}

remove_registered() {
	local v
	for v in $(registered_versions); do
		echo "Removing ${NAME}/${v} from DKMS"
		${SUDO} dkms remove -m "${NAME}" -v "${v}" --all ||
			warn "'dkms remove -m ${NAME} -v ${v} --all' failed"
	done
	# Source trees left over from earlier installs.
	local d
	for d in /usr/src/"${NAME}"-*/; do
		[[ -f "${d}dkms.conf" ]] || continue
		grep -q "^PACKAGE_NAME=\"${NAME}\"" "${d}dkms.conf" || continue
		${SUDO} rm -rf "${d}"
	done
}

# Unloads casfs, retrying because an unmount can take a moment to drop its
# module reference. Returns non-zero if casfs is still loaded.
unload() {
	local i
	for i in $(seq 1 5); do
		[[ -d /sys/module/${NAME} ]] || return 0
		${SUDO} rmmod "${NAME}" 2>/dev/null && return 0
		sleep 1
	done
	[[ ! -d /sys/module/${NAME} ]]
}

# Loads the DKMS-installed module, replacing an older loaded casfs if nothing
# is using it.
load() {
	local want loaded
	# srcversion is a checksum of the sources, but it does not cover
	# version.h, so compare the version too.
	want="$(modinfo -F version "${NAME}") $(modinfo -F srcversion "${NAME}")"
	if [[ -d /sys/module/${NAME} ]]; then
		loaded="$(cat /sys/module/${NAME}/version /sys/module/${NAME}/srcversion 2>/dev/null |
			paste -sd ' ' || true)"
		if [[ "${loaded}" == "${want}" ]]; then
			echo "${NAME} ($(cat /sys/module/${NAME}/version)) is already loaded."
			return 0
		fi
		if ! unload; then
			warn "${NAME} is in use; keeping the currently loaded module."
			warn "Unmount all casfs file systems and run 'sudo modprobe -r ${NAME} && sudo modprobe ${NAME}' to load the new module."
			return 0
		fi
	fi
	${SUDO} modprobe "${NAME}"
	echo "${NAME} ($(cat /sys/module/${NAME}/version)) loaded."
}

install() {
	local version="$1"
	command -v dkms >/dev/null ||
		die "dkms not found; install it first (e.g. 'sudo apt-get install dkms')."
	[[ -f "${SRC_DIR}/version.h" ]] ||
		die "${SRC_DIR}/version.h is missing; run 'make dkms-install' instead."
	if dpkg-query -W -f='${Status}' "${NAME}-dkms" 2>/dev/null |
		grep -q 'ok installed'; then
		die "The ${NAME}-dkms package is installed; remove it before installing from source."
	fi

	remove_registered

	local dest="/usr/src/${NAME}-${version}"
	echo "Copying the ${NAME} sources to ${dest}"
	${SUDO} mkdir -p "${dest}"
	# Everything Kbuild needs to build the module (./*.h includes the
	# generated version.h).
	(cd "${SRC_DIR}" && ${SUDO} cp -r Makefile Kconfig LICENSE VERSION \
		version.h.template ./*.c ./*.h ctrl makefiles "${dest}/")
	${SUDO} find "${dest}" \( -name '*.o' -o -name '*.cmd' -o -name '*.ko' \
		-o -name '*.mod*' -o -name '*.a' \) -delete
	sed -e "s/#MODULE_VERSION#/${version}/g" "${SRC_DIR}/dkms.conf" |
		${SUDO} tee "${dest}/dkms.conf" >/dev/null

	${SUDO} dkms add -m "${NAME}" -v "${version}"

	# Build for every installed kernel with headers, so that a kernel that was
	# installed but not booted yet (e.g. by unattended-upgrades) also gets
	# the module. DKMS takes care of kernels installed later.
	local running build_dir kver
	running="$(uname -r)"
	for build_dir in /lib/modules/*/build; do
		[[ -e "${build_dir}/Makefile" ]] || continue
		kver="$(basename "$(dirname "${build_dir}")")"
		echo "Building ${NAME}/${version} for kernel ${kver}"
		if ${SUDO} dkms build -m "${NAME}" -v "${version}" -k "${kver}" &&
			${SUDO} dkms install -m "${NAME}" -v "${version}" -k "${kver}"; then
			continue
		fi
		[[ "${kver}" != "${running}" ]] ||
			die "Building ${NAME} for the running kernel failed; see the make.log path above."
		warn "Building ${NAME} for kernel ${kver} failed; continuing."
	done
	[[ -e "/lib/modules/${running}/build/Makefile" ]] ||
		die "No headers for the running kernel ${running}; install linux-headers-${running}."

	echo "Loading ${NAME} at boot (${MODULES_LOAD_CONF})"
	echo "${NAME}" | ${SUDO} tee "${MODULES_LOAD_CONF}" >/dev/null

	load
	echo
	${SUDO} dkms status -m "${NAME}"
}

uninstall() {
	if [[ -d /sys/module/${NAME} ]] && ! unload; then
		warn "${NAME} is in use and stays loaded until the next reboot."
	fi
	if command -v dkms >/dev/null; then
		remove_registered
	fi
	${SUDO} rm -f "${MODULES_LOAD_CONF}"
	echo "${NAME} removed from DKMS."
}

case "${1:-}" in
install)
	[[ $# -eq 2 && -n "$2" ]] || usage
	install "$2"
	;;
uninstall)
	[[ $# -eq 1 ]] || usage
	uninstall
	;;
*)
	usage
	;;
esac
