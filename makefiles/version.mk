# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 Google LLC

CASFS_VERSION := $(shell $(dir $(lastword $(MAKEFILE_LIST)))/get_version.sh)
ifeq ($(CASFS_VERSION), )
$(error Failed to get version)
endif
