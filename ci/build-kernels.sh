#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Google LLC
#
# Builds casfs.ko against the headers of one or more kernel versions, using the
# prebuilt header packages from the Ubuntu mainline kernel archive
# (https://kernel.ubuntu.com/mainline/). This is what CI runs, and it can be run
# locally the same way. It only checks that the module compiles and links; it
# does not load it.
#
# Usage: ci/build-kernels.sh [options] [KERNEL...]
#
# KERNEL is one of:
#   X.Y            the newest X.Y.Z release that has amd64 headers, e.g. 6.12
#   X.Y.Z, X.Y-rcN exactly that release, e.g. 6.12.112 or 7.3-rc3
#   latest         the newest release, or a newer -rc if there is one
#   host           the headers of the running kernel (/lib/modules/$(uname -r))
# With no KERNEL arguments, the list in ci/kernels.txt is used.
#
# Options:
#   -o DIR  output directory for sources, objects and logs
#           (default: out/kernels in the repository)
#   -c DIR  download cache for header packages
#           (default: ${XDG_CACHE_HOME:-~/.cache}/casfs-kernel-headers)
#   -j N    total number of parallel compile jobs (default: nproc)
#   -W      fail if the casfs sources produce compiler or modpost warnings
#   -h      show this help
#
# Requires: bash, curl, dpkg-deb, flock, gcc, git, make, sha256sum.

set -euo pipefail

SRC_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
MAINLINE_URL=${MAINLINE_URL:-https://kernel.ubuntu.com/mainline}
ARCH=amd64

OUT=$SRC_ROOT/out/kernels
CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/casfs-kernel-headers
JOBS=$(nproc)
WERROR=0

usage() {
	sed -n '/^# Usage:/,/^# Requires:/s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
}

die() {
	echo "error: $*" >&2
	exit 1
}

while getopts "o:c:j:Wh" opt; do
	case $opt in
	o) OUT=$(realpath -m "$OPTARG") ;;
	c) CACHE=$(realpath -m "$OPTARG") ;;
	j) JOBS=$OPTARG ;;
	W) WERROR=1 ;;
	h) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
done
shift $((OPTIND - 1))

if [[ $# -gt 0 ]]; then
	KERNELS=("$@")
else
	mapfile -t KERNELS < <(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' \
		-e 's/[[:space:]]//g' "$SRC_ROOT/ci/kernels.txt")
fi
[[ ${#KERNELS[@]} -gt 0 ]] || die "no kernels to build"

for tool in curl dpkg-deb flock gcc git make sha256sum; do
	command -v "$tool" >/dev/null || die "'$tool' is required"
done

mkdir -p "$OUT" "$CACHE"
INDEX=$OUT/.mainline-index
CASFS_VERSION=$("$SRC_ROOT/makefiles/get_version.sh")

fetch() {
	curl -fsSL --retry 3 --retry-delay 2 "$@"
}

# Lists every version in the mainline archive, oldest first, without the
# leading "v" (e.g. "6.12.112", "7.3-rc3").
fetch_index() {
	fetch "$MAINLINE_URL/" |
		grep -oE 'href="v[0-9]+\.[0-9]+(\.[0-9]+)?(-rc[0-9]+)?/"' |
		sed -E 's/^href="v//; s|/"$||' | sort -uV >"$INDEX.tmp"
	[[ -s $INDEX.tmp ]] || die "could not list $MAINLINE_URL/"
	mv "$INDEX.tmp" "$INDEX"
}

# Prints the names of the two header packages for a version, or nothing if
# the version has no amd64 headers (e.g. the mainline build failed).
header_debs() {
	fetch "$MAINLINE_URL/v$1/$ARCH/" 2>/dev/null |
		grep -oE "href=\"linux-headers-[^\"]+-generic_[^\"]+_$ARCH\.deb\"|href=\"linux-headers-[^\"]+_all\.deb\"" |
		sed -E 's/^href="//; s/"$//' | sort -u || true
}

# Is version $1 newer than version $2?
newer() {
	[[ $1 != "$2" && $(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1) == "$1" ]]
}

# Resolves a KERNEL argument to an exact version that has headers.
resolve() {
	local spec=$1 candidates rel rc v
	case $spec in
	latest)
		rel=$(grep -v -- -rc "$INDEX" | tail -1)
		rc=$(grep -- -rc "$INDEX" | tail -1 || true)
		candidates=$(grep -v -- -rc "$INDEX" | sort -rV)
		if [[ -n $rc ]] && newer "${rc%-rc*}" "$(cut -d. -f1,2 <<<"$rel")"; then
			candidates=$(printf '%s\n%s\n' "$rc" "$candidates")
		fi
		;;
	[0-9]*.[0-9]*)
		if [[ $spec =~ ^[0-9]+\.[0-9]+$ ]]; then
			candidates=$(grep -E "^${spec//./\\.}(\.[0-9]+)?$" "$INDEX" | sort -rV || true)
		else
			candidates=$spec
		fi
		;;
	*)
		die "unknown kernel '$spec'"
		;;
	esac
	# Mainline builds occasionally fail, so fall back to a few older releases.
	for v in $(head -5 <<<"$candidates"); do
		if [[ $(header_debs "$v" | wc -l) -eq 2 ]]; then
			echo "$v"
			return
		fi
		echo "note: v$v has no $ARCH headers, trying an older release" >&2
	done
	die "no $ARCH headers found for '$spec'"
}

# Downloads, verifies and unpacks the headers for a version, and prints the
# directory to build against.
fetch_headers() {
	local ver=$1 dir=$CACHE/$1 deb sum kdir
	if [[ -f $dir/.kdir && -d $(cat "$dir/.kdir") ]]; then
		cat "$dir/.kdir"
		return
	fi
	rm -rf "$dir"
	mkdir -p "$dir/debs" "$dir/root"
	fetch -o "$dir/CHECKSUMS" "$MAINLINE_URL/v$ver/$ARCH/CHECKSUMS"
	for deb in $(header_debs "$ver"); do
		echo "downloading $deb" >&2
		fetch -o "$dir/debs/$deb" "$MAINLINE_URL/v$ver/$ARCH/$deb"
		sum=$(awk -v f="$deb" '/^# Checksums-Sha256/ { s = 1; next }
			/^#/ { s = 0 } s && $2 == f { print $1 }' "$dir/CHECKSUMS")
		[[ -n $sum ]] || die "no SHA-256 checksum for $deb"
		echo "$sum  $dir/debs/$deb" | sha256sum -c --quiet - >&2 ||
			die "checksum mismatch for $deb"
		dpkg-deb -x "$dir/debs/$deb" "$dir/root"
	done
	rm -rf "$dir/debs"
	kdir=$(compgen -G "$dir/root/usr/src/linux-headers-*-generic" | head -1)
	[[ -n $kdir && -f $kdir/Makefile ]] || die "unexpected package layout in $dir"
	echo "$kdir" >"$dir/.kdir"
	echo "$kdir"
}

# Copies the module sources (tracked and untracked-but-not-ignored files, so
# local edits are included) into a per-kernel directory, so that builds for
# different kernels don't share object files.
copy_sources() {
	local dest=$1
	rm -rf "$dest"
	mkdir -p "$dest"
	git -C "$SRC_ROOT" ls-files -z --cached --others --exclude-standard -- \
		':!.github' ':!api' ':!ci' ':!docs' ':!hooks' ':!out' ':!tests' ':!third_party' |
		(cd "$SRC_ROOT" && while IFS= read -r -d '' f; do
			if [[ -e $f ]]; then printf '%s\0' "$f"; fi
		done | xargs -0 -r cp --parents -t "$dest")
	sed "s|\$VERSION|$CASFS_VERSION|" "$SRC_ROOT/version.h.template" \
		>"$dest/version.h"
}

# Prints compiler diagnostics that point into the casfs sources rather than
# into the kernel headers. Kbuild reports casfs paths either relative to the
# module directory (6.13+) or as absolute paths under it (older kernels).
casfs_diagnostics() {
	local log=$1 src=$2 kind=$3
	grep -E "^[^ :]+:[0-9]+:[0-9]+: $kind:" "$log" |
		sed "s|^$src/||" |
		grep -vE "^(/|\./(include|arch|scripts|tools)/|(include|arch|scripts|tools)/)" |
		sort -u || true
}

# Builds casfs for one KERNEL argument. Writes the build log and a one-line
# result ("<status> <headers> <warnings>") to the output directory.
build_one() {
	local spec=$1 jobs=$2 dir=$OUT/$1 krel status warnings rc
	mkdir -p "$dir"
	rm -f "$dir/result" "$dir/krel"
	# Run the steps in a subshell with errexit, and check its status
	# separately: errexit is ignored inside "a && b || c" lists.
	set +e
	(
		set -e
		if [[ $spec == host ]]; then
			kdir=/lib/modules/$(uname -r)/build
			[[ -f $kdir/Makefile ]] || die "no headers for the running kernel at $kdir"
			uname -r >"$dir/krel"
		else
			ver=$(resolve "$spec")
			echo "building against v$ver"
			exec {lock}>"$CACHE/.lock-$ver"
			flock "$lock"
			kdir=$(fetch_headers "$ver")
			flock -u "$lock"
			basename "$kdir" | sed 's/^linux-headers-//' >"$dir/krel"
		fi
		echo "headers: $kdir"
		copy_sources "$dir/src"
		make -C "$kdir" M="$dir/src" -j"$jobs" modules
	) >"$dir/build.log" 2>&1
	rc=$?
	set -e
	status=ok
	[[ $rc -eq 0 && -f $dir/src/casfs.ko ]] || status=FAILED

	krel=$(cat "$dir/krel" 2>/dev/null || echo -)
	warnings=$(( $(casfs_diagnostics "$dir/build.log" "$dir/src" warning | wc -l) +
		$(grep -c '^WARNING: modpost:' "$dir/build.log" || true) ))
	if [[ $status == ok && $WERROR == 1 && $warnings -gt 0 ]]; then
		status=WARNINGS
	fi
	echo "$status $krel $warnings" >"$dir/result"
}

# Turns casfs diagnostics into GitHub Actions annotations on the source lines.
annotate() {
	local log=$1 src=$2 kind line file lno msg
	for kind in error warning; do
		while IFS= read -r line; do
			IFS=: read -r file lno _ msg <<<"$line"
			msg=${msg#*"$kind": }
			echo "::$kind file=$file,line=$lno::[$3] $msg"
		done < <(casfs_diagnostics "$log" "$src" "$kind")
	done
}

if [[ " ${KERNELS[*]} " != " host " ]]; then
	fetch_index
fi

per_build=$(( JOBS / ${#KERNELS[@]} ))
(( per_build >= 1 )) || per_build=1

echo "casfs $CASFS_VERSION: building for ${KERNELS[*]} (logs in $OUT)"
pids=()
for k in "${KERNELS[@]}"; do
	build_one "$k" "$per_build" &
	pids+=($!)
done
for pid in "${pids[@]}"; do
	wait "$pid" || true
done

failed=0
summary=$(printf '%-8s %-28s %-9s %-8s %s\n' KERNEL HEADERS RESULT WARNINGS LOG)
md="| Kernel | Headers | Result | Warnings |"$'\n'"| :--- | :--- | :--- | ---: |"
for k in "${KERNELS[@]}"; do
	dir=$OUT/$k
	if [[ -f $dir/result ]]; then
		read -r status krel warnings <"$dir/result"
	else
		status=FAILED krel=- warnings=-
	fi
	[[ $status == ok ]] || failed=1
	summary+=$'\n'$(printf '%-8s %-28s %-9s %-8s %s' "$k" "$krel" "$status" \
		"$warnings" "${dir#"$SRC_ROOT"/}/build.log")
	md+=$'\n'"| $k | $krel | $status | $warnings |"
	if [[ -n ${GITHUB_ACTIONS:-} ]]; then
		annotate "$dir/build.log" "$dir/src" "$k"
		if [[ $status != ok ]]; then
			echo "::group::build log for $k"
			cat "$dir/build.log"
			echo "::endgroup::"
		fi
	elif [[ $status != ok ]]; then
		echo "---- last 40 lines of $dir/build.log ----"
		tail -40 "$dir/build.log"
	fi
done

echo
echo "$summary"
if [[ -n ${GITHUB_STEP_SUMMARY:-} ]]; then
	echo "$md" >>"$GITHUB_STEP_SUMMARY"
fi
exit "$failed"
