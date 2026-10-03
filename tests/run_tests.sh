#! /usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024 Google LLC

set -e

SCRIPT_DIR="$(realpath $(dirname "$0"))"

for test in $SCRIPT_DIR/*_test.sh; do
  echo "Running test $test..."
  if ! $test; then
    echo "FAILED: $test"
    exit 1
  fi
done
