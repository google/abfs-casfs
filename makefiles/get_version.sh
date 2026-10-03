#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Google LLC

# This script returns the CASFS version with a priority system:
# 1. If the CASFS_VERSION environment variable is set, that is used.
# 2. If 'git describe' returns something, use that.
# 3. Otherwise the version in the top-level VERSION file is used.

set -e

# From ENV variable.
if [[ -n "$CASFS_VERSION" ]]; then
  echo -n "$CASFS_VERSION"
  exit 0
fi

# From Git.
GIT_VERSION=$(git describe HEAD 2> /dev/null || true)
if [[ -n "$GIT_VERSION" ]]; then
  CASFS_VERSION="$GIT_VERSION"

  if [[ ! "$CASFS_VERSION" =~ v[0-9]+\.[0-9]+\.[0-9]+ ]]; then
    echo "Error: $CASFS_VERSION does not follow semantic versioning with a leading 'v'" >&2
    exit 1
  fi

  SUFFIX=""
  if ! git diff --quiet; then
    SUFFIX="-dirty"
  fi
  CASFS_VERSION="$CASFS_VERSION$SUFFIX"
  CASFS_VERSION=$(echo "$CASFS_VERSION" | sed 's/^v//')

  echo -n "$CASFS_VERSION"
  exit 0
fi

# From the VERSION file.
CASFS_VERSION=$(tr -d '[:space:]' < "$(dirname "$0")/../VERSION")

echo -n "$CASFS_VERSION"
