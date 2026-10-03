# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2023 Google LLC

SHELL := /bin/bash

FIRST_MAKEFILE := $(abspath $(firstword $(MAKEFILE_LIST)))
THIS_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))

# The VERSION defined here should only be applicable when this Makefile is
# the top-level Makefile and the "version.h" rule is ran. This also limits the
# scope of the variables inside this conditional so that they don't leak into
# the varspace of external Makefiles including this Makefile.
ifeq ($(FIRST_MAKEFILE), $(THIS_MAKEFILE))
include makefiles/version.mk
endif # ifeq ($(FIRST_MAKEFILE), $(THIS_MAKEFILE))

KERNEL_RELEASE ?= $(shell uname -r)

# Compiler & Kbuild flags
KBUILD_CFLAGS += -g -Wall
ccflags-y     := -std=gnu11 -Wall -Wframe-larger-than=1024 \
                 -Wno-missing-declarations -Wno-missing-prototypes \
                 $(call cc-option,-Xassembler --gsframe=no)

obj-m := casfs.o

OBJECT_FILES_NON_STANDARD_casfs.o := y

root_objs := \
	cache.o \
	cas_ctrl.o \
	cas_tap.o \
	cas_zlib.o \
	dir.o \
	dir_ioctl.o \
	tree.o \
	deeptree.o \
	fstream.o \
	dir_rb_tree.o \
	file.o \
	blob.o \
	fs.o \
	hash.o \
	kcasfs.o \
	node.o \
	provider.o \
	signal.o \
	sysfs.o

ctrl_objs := 	$(addprefix ctrl/, \
		inode.o \
		util.o \
		)

tap_objs  :=	$(addprefix ctrl/tap/, \
		ctrl.o \
		session.o \
		tap.o \
		inode.o \
		)

git_objs  :=	$(addprefix ctrl/git/, \
		ctrl.o \
		objects.o \
		)

casfs-y := $(root_objs) $(ctrl_objs) $(tap_objs) $(git_objs)

.PHONY: all ko reload checkpatch-diff checkpatch-head checkpatch-all \
        version api apitests install install-hooks clean

all: install-hooks ko api apitests

version:

# We only want this rule to do something when the casfs makefile
# is the top makefile
git-version: version
	@if [[ "$(THIS_MAKEFILE)" == "$(FIRST_MAKEFILE)" ]]; then \
		echo "$(CASFS_VERSION)"; \
		if [[ "$(CASFS_VERSION)" != "`cat git-version 2> /dev/null`" ]]; then echo $(CASFS_VERSION) > git-version; fi; \
	fi

version.h: version.h.template git-version
	VERSION=$(CASFS_VERSION) envsubst < version.h.template > version.h

ko: version.h
	$(MAKE) -C /lib/modules/$(KERNEL_RELEASE)/build M=$(CURDIR) modules

reload: ko
	-sudo rmmod casfs 2>/dev/null || true
	sudo insmod casfs.ko

install: ko
	mkdir -p $(DESTDIR)/lib/modules/$(KERNEL_RELEASE)/kernel/fs/casfs
	cp casfs.ko $(DESTDIR)/lib/modules/$(KERNEL_RELEASE)/kernel/fs/casfs

apitests:
	go test -C api/tests -c

api:
	go build -C api/cmd -o casfsapi

checkpatch-diff:
	@git diff HEAD | ./third_party/linux/scripts/checkpatch.pl  -

checkpatch-head:
	./third_party/linux/scripts/checkpatch.pl -g HEAD

# This isn't gating anything, but can be ran at anytime to lint the
# entire repo.
checkpatch-all:
	find . -type f \( -name "*.h" -o -name "*.c" \) -exec ./third_party/linux/scripts/checkpatch.pl  -q -f {} +

install-hooks:
	@if ! cmp -s hooks/pre-commit .git/hooks/pre-commit; then \
		mkdir -p .git/hooks; \
		cp hooks/pre-commit .git/hooks/.; \
		chmod +x .git/hooks/pre-commit; \
	fi
	@if ! cmp -s hooks/pre-push .git/hooks/pre-push; then \
		mkdir -p .git/hooks; \
		cp hooks/pre-push .git/hooks/.; \
		chmod +x .git/hooks/pre-push; \
	fi


clean:
	@echo "## This command temporarily moves api/go.mod to protect it from 'find' pattern match ##"
	@echo "## If this rule interrupts or early terminates, you may need to manually restore the file ##"
	@echo ""
	@if [[ -f api/go.mod ]]; then mv api/go.mod api/go.mod.tmp; fi
	$(MAKE) -C /lib/modules/$(KERNEL_RELEASE)/build M=$(CURDIR) clean
	@if [[ -f api/go.mod.tmp ]]; then mv api/go.mod.tmp api/go.mod; fi
	rm -f version.h git-version api/cmd/casfsapi api/tests/*.test
