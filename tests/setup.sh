#! /usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024 Google LLC

check_cmd()
{
  if ! command -v "$1" > /dev/null; then
    echo "$1 is missing. Please install and try again."
    return 1
  fi
}
check_setfattr()
{
  check_cmd setfattr
  return $?
}
check_getfattr()
{
  check_cmd getfattr
  return $?
}

get_mnt_dir()
{
  echo $(mktemp -d /tmp/casfs_test.XXXXXXXXXX)
}
get_cache_dir()
{
  echo $(mktemp -d /tmp/casfs_test_cache.XXXXXXXXXX)
}

mount()
{
  local extra_opts=""
  if [ -n "$3" ]; then
    extra_opts="$3"
  fi
  echo "Mounting $1 with cache $2 $extra_opts"

  mkdir -p "$1"

  sudo mount -t casfs none "$1" -o cache="$2"  -o local $extra_opts
  if [ $? -ne 0 ]; then
    echo "Failed to mount $1 with cache $2"
    return 1
  fi

  return $?
}

unmount()
{
  echo "Unmounting $1"
  sudo umount $1
  return $?
}

cleanup_mount()
{
  if [ -z "$1" ]; then
    echo "Invalid mount"
    return 1
  fi
  rmdir "$1"
}

read_hash()
{
  echo $(getfattr --absolute-names --only-values -n casfs.hash "$1")
}

set_hash()
{
  setfattr -n casfs.hash "$1" -v "$2"
  return $?
}

set_deeptree_hash()
{
  setfattr -n casfs.deeptree "$1" -v "$2"
  return $?
}
