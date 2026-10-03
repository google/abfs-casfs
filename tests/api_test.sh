#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2025 Google LLC

set -e

source $(dirname "$0")/setup.sh

ROOT_DIR=$(git rev-parse --show-toplevel)
MNT_DIR=$(get_mnt_dir)
CACHE_DIR=$(get_cache_dir)
NON_CAS_DIR=$(mktemp -d /tmp/non-casfs_test.XXXXXXXXXX)
do_cleanup=1

# Flag to control privileged integration testing (Default: 0 / Off)
RUN_INTEGRATION_EXT=0

# Parse incoming arguments to check for the flag toggle
while [[ $# -gt 0 ]]; do
  case $1 in
    --run-integration)
      RUN_INTEGRATION_EXT=1
      shift
      ;;
    *)
      echo "Unknown option: $1"
      exit 1
      ;;
  esac
done

DISK_DIR=$(mktemp -d /tmp/casfs-backing-dir.XXXXXXXXXX)
DISK_IMG=$DISK_DIR/img
LOOP_DIR=$(sudo losetup -f)

setup_loop()
{
  if [[ -z $LOOP_DIR ]]; then
    echo "no loop directory detected skipping"
    return
  fi
  echo "Running with loop $LOOP_DIR"
  dd if=/dev/zero of=$DISK_IMG bs=1024 count=1024000
  sudo losetup $LOOP_DIR $DISK_IMG
  sudo mkfs.ext4 $LOOP_DIR
  sudo mount $LOOP_DIR $CACHE_DIR
  sudo chown $USER:$GROUP $CACHE_DIR
}

cleanup_loop()
{
  if [[ -z $LOOP_DIR ]]; then
    rm -rf $CACHE_DIR
    return
  fi
  echo "Unmounting cache $CACHE_DIR on loopback"
  sudo umount $CACHE_DIR
  rm $DISK_IMG
  rmdir $DISK_DIR
  rmdir  $CACHE_DIR
  sudo losetup -d $LOOP_DIR
}

cleanup()
{
  if [ $do_cleanup == 1 ]; then
    unmount $MNT_DIR
    rmdir $MNT_DIR
    rm -rf $NON_CAS_DIR
    cleanup_loop
  fi
}

setup_loop
trap "cleanup" ERR
mount $MNT_DIR $CACHE_DIR
if [ $? -ne 0 ]; then
  exit $?
fi

COVERPACKAGES=../,../xattr,../hash,../tapping

run_casfsapi_tests()
{
   local test_filter="$1"
   echo "Running api tests with filter: ${test_filter:-all}"
   go test -C $ROOT_DIR/api/tests/ $test_filter -coverprofile=apicoverage.out -coverpkg=$COVERPACKAGES -v  -args -root $MNT_DIR  -objectsDir $CACHE_DIR/objects -blobsDir $CACHE_DIR/blobs -scratchDir $CACHE_DIR/scratch -arisDir $CACHE_DIR/aris -nonCasDir $NON_CAS_DIR -cli $ROOT_DIR/api/cmd/casfsapi
}

# -------------------------------------------------------------------------
# Privileged Integration Runner with Automatic Restoration Trap
# -------------------------------------------------------------------------
run_integration_tests()
{
   local override_path="/sys/kernel/debug/casfs/max_tap_entries_override"
   local param_path="/sys/module/casfs/parameters/casfs_max_tap_entries"

   # Use sudo to evaluate file presence securely, avoiding debugfs mount permission drops
   if ! sudo test -f "$override_path"; then
       echo "Skipping integration: debugfs module override path not available or debugfs not mounted."
       return 0
   fi

   # Capture the original production limit into a local variable
   local original_max_entries=$(cat "$param_path")
   if [ -z "$original_max_entries" ]; then
       echo "Error: Failed to read original max tap entries. Aborting integration tests safely."
       return 1
   fi
   echo "Captured original production tap limit: $original_max_entries"

   # If someone hits Ctrl+C (SIGINT) or the script throws an unhandled error right now, 
   # ensure we restore the ceiling before we drop out.
   trap "echo 'Emergency abort caught! Restoring...'; sudo sh -c 'echo $original_max_entries > $override_path'" INT ERR

   echo "Running privileged memory guardrail integration tests..."
   sudo -E go test -C $ROOT_DIR/api/integration/ -v -tags=integration_root . -args -mntDir "$MNT_DIR"
   local test_res=$?

   echo "Ensuring module restoration: Resetting tap ceiling back to $original_max_entries"
   sudo sh -c "echo $original_max_entries > $override_path"

   # Clear the emergency trap so it doesn't cross over into the global cleanup phase
   trap - INT ERR

   return $test_res
}

run_casfsapi_tests
casfsapi_test_ret=$?


if [ $casfsapi_test_ret -eq 0 ] && [ $RUN_INTEGRATION_EXT -eq 1 ]; then
  run_integration_tests || casfsapi_test_ret=$?
fi

cleanup

exit $casfsapi_test_ret
