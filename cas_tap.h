/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google LLC
 */

#ifndef _CAS_TAP_H
#define _CAS_TAP_H

#include <linux/file.h>
#include <linux/dcache.h>

#include "node.h"

extern void cas_tap_mkdir(struct dentry *dentry);
extern void cas_tap_symlink(struct dentry *dentry, const char *link);
extern void cas_tap_symlink_read(struct dentry *dentry);
extern void cas_tap_link(struct dentry *source, struct dentry *target);
extern void cas_tap_unlink(struct dentry *dentry);
extern void cas_tap_rename(struct dentry *old, struct dentry *new);
extern void cas_tap_getattr(struct dentry *dentry);
extern void cas_tap_attr_change(struct dentry *dentry);

extern void cas_tap_readdir(struct file *file);
extern void cas_tap_open(struct file *file, bool read_only);

#endif /*_CAS_TAP_H*/
