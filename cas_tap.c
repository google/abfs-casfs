// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 */

#include "fs.h"
#include "node.h"
#include "cas_tap.h"

#include "ctrl/tap/ctrl.h"

#define D_TO_TAP(dentry) SB_TO_FS(dentry->d_sb)->ctrl->tap
#define F_TO_TAP(file) SB_TO_FS(file_inode(file)->i_sb)->ctrl->tap

#define D_TO_N(dentry) INODE_TO_CAS(d_inode(dentry))
#define F_TO_N(file) INODE_TO_CAS(file_inode(file))

void cas_tap_mkdir(struct dentry *dentry)
{
	struct tap_ctrl *tap = D_TO_TAP(dentry);

	if (tap)
		tap_mkdir(tap, D_TO_N(dentry), dentry);
}
void cas_tap_symlink(struct dentry *dentry, const char *link)
{
	struct tap_ctrl *tap = D_TO_TAP(dentry);

	if (tap)
		tap_symlink(tap, D_TO_N(dentry), dentry, link);
}
void cas_tap_symlink_read(struct dentry *dentry)
{
	struct tap_ctrl *tap = D_TO_TAP(dentry);

	if (tap)
		tap_symlink_read(tap, D_TO_N(dentry), dentry);
}

void cas_tap_link(struct dentry *source, struct dentry *target)
{
	struct tap_ctrl *tap = D_TO_TAP(source);

	if (tap)
		tap_link(tap, D_TO_N(source), source, target);
}

void cas_tap_unlink(struct dentry *dentry)
{
	struct tap_ctrl *tap = D_TO_TAP(dentry);

	if (tap)
		tap_unlink(tap, D_TO_N(dentry), dentry);
}
void cas_tap_getattr(struct dentry *dentry)
{
	struct tap_ctrl *tap = D_TO_TAP(dentry);

	if (tap)
		tap_getattr(tap, D_TO_N(dentry), dentry);
}
void cas_tap_rename(struct dentry *old, struct dentry *new)
{
	struct tap_ctrl *tap = D_TO_TAP(old);
	struct cas_node *new_node = NULL;

	if (d_really_is_positive(new))
		new_node = D_TO_N(new);

	if (tap)
		tap_rename(tap, D_TO_N(old), old, new_node, new);
}
void cas_tap_readdir(struct file *file)
{
	struct tap_ctrl *tap = F_TO_TAP(file);

	if (tap)
		tap_readdir(tap, F_TO_N(file), file);
}

void cas_tap_open(struct file *file, bool read_only)
{
	struct tap_ctrl *tap = F_TO_TAP(file);

	if (tap)
		tap_file_open(tap, F_TO_N(file), file, read_only);
}

void cas_tap_attr_change(struct dentry *dentry)
{
	struct tap_ctrl *tap = D_TO_TAP(dentry);

	if (tap)
		tap_attr_change(tap, D_TO_N(dentry), dentry);
}
