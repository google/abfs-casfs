#! /usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024 Google LLC

source $(dirname "$0")/setup.sh

check_setfattr
if [ $? -ne 0 ]; then
   return $?
fi
check_getfattr
if [ $? -ne 0 ]; then
   return $?
fi

MNT_DIR=$(get_mnt_dir)
CACHE_DIR=$(get_cache_dir)
do_cleanup=1

cleanup()
{
  if [ $do_cleanup == 1 ]; then
    unmount $MNT_DIR
    rm -rf $CACHE_DIR
    rmdir $MNT_DIR
  fi
}

# Populate directory with given number of files
create_directory_files()
{
  for i in $(seq  "$2");
  do
    echo "Creating file $i" > "$1/$i"
  done
}

replay_directory()
{
  # Mount new directory  with hash ($1), check if
  # file count matches expected ($2)
  replay_dir=$(get_mnt_dir)
  mount $replay_dir $CACHE_DIR
  if [ $? -ne 0 ]; then
    return $?
  fi
  set_hash $replay_dir $1
  let rc=$?
  if [ $rc -ne 0 ]; then
    unmount $replay_dir
    return $rc
  fi

  replay_file_count=$(ls -1 $replay_dir | wc -l)

  unmount $replay_dir

  if [ $replay_file_count != $2 ]; then
    echo "Replay mount $1 contains $replay_file_count files, but expected $2"
    return 1
  fi

}

run_basic_dir_test()
{
  # Create 10 files in the directory where we have already
  # stored away the hash to compare to.
  echo "Creating known directory"
  create_directory_files "$MNT_DIR" 10
  exp_hash="54a6ec43a6aff162d8837b8faa0454078db0c824"
  hash=$(read_hash "$MNT_DIR")
  if [[ "$hash" != "$exp_hash" ]]; then
    echo "Expected hash $exp_hash got $hash"
    return 1
  fi

  replay_directory $hash 10
  return $?
}

run_large_dir_test()
{
  file_count=200000
  echo "Creating directory with $file_count given files"

  create_directory_files "$MNT_DIR" $file_count

  # Read the hash
  hash=$(read_hash "$MNT_DIR")

  replay_directory $hash $file_count
  return $?
}

run_invalid_deeptree()
{
  dir=$(get_mnt_dir)
  mount $dir $CACHE_DIR
  if [ $? -ne 0 ]; then
    return $?
  fi
  hash="9999999999999999999999999999999999999999"
  set_deeptree_hash $dir $hash 2>/dev/null
  let rc=$?
  if [ $rc -ne 0 ]; then
    unmount $dir
    return 0
  fi

  unmount $dir
  return 1
}

run_invalid_tree()
{
  dir=$(get_mnt_dir)
  mount $dir $CACHE_DIR
  if [ $? -ne 0 ]; then
    return $?
  fi
  hash="9999999999999999999999999999999999999999"
  set_hash $dir $hash
  let rc=$?
  # set hash does returns 0 as long as the hash value is
  # correct structure.  failure won't be reported until
  # directory is accessed below
  if [ $rc -ne 0 ]; then
    unmount $dir
    return 1
  fi

  ls $dir 2>/dev/null
  rc=$?
  if [ $rc -ne 0 ]; then
    unmount $dir
    return 0
  fi

  unmount $dir
  return 1
}

print_result()
{
  code="OK"
  if [ $2 -ne 0 ]; then
    code="FAILED"
  fi
  echo "$1   : $code"
}


mount $MNT_DIR $CACHE_DIR
if [ $? -ne 0 ]; then
  exit $?
fi


run_basic_dir_test
basic_ret=$?

run_large_dir_test
large_ret=$?

run_invalid_tree
invalid_tree_ret=$?

run_invalid_deeptree
invalid_deeptree_ret=$?

cleanup

echo ""
print_result "basic directory test" $basic_ret
print_result "large directory test" $large_ret
print_result "invalid tree" $invalid_tree_ret
print_result "invalid deeptree test" $invalid_deeptree_ret

exit $basic_ret || $large_ret || $invalid_deeptree_ret || $invalid_tree_ret
