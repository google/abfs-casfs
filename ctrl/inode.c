// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 *
 * Adapted from fs/ramfs/inode.c in the Linux kernel, which carries the
 * following notice:
 *
 *   Resizable simple ram filesystem for Linux.
 *
 *   Copyright (C) 2000 Linus Torvalds.
 *                 2000 Transmeta Corp.
 *
 *   Usage limits added by David Gibson, Linuxcare Australia.
 *   This file is released under the GPL.
 */
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/ramfs.h>
#include <linux/sched.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/seq_file.h>

#include <linux/mm.h>
#include <linux/sched.h>

#include "inode.h"
#include "tap/inode.h"
#include "git/inode.h"
#include "../fstream.h"
#include "../compat.h"
#include "../compat_inode.h"

/* Control provides user read-only directory */
const struct inode_operations ctrlfs_dir_inode_operations = {
	.lookup = simple_lookup,
	.rmdir = simple_rmdir,
	.unlink = simple_unlink,
};

const struct inode_operations ctrlfs_file_inode_operations = {
	.setattr = simple_setattr,
	.getattr = simple_getattr,
};

struct inode *cas_ctrl_iget(struct super_block *sb, const struct inode *dir,
			    umode_t mode, enum cas_ctrl_type type)
{
	struct cas_node *node;
	struct inode *inode = cas_new_inode_locked(sb);

	if (!inode)
		return NULL;

	node = INODE_TO_CAS(inode);
	node->type = CAS_TYPE_CTRL;
	node->cas_ctrl_node.type = type;
	inode->i_mode = mode;
	inode->i_mapping->a_ops = &ram_aops;
	mutex_init(&node->lock);
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
	mapping_set_unevictable(inode->i_mapping);
	set_nlink(inode, 1);

	switch (type) {
	case CAS_CTRL_TYPE_DIR:
		/* Don't allow any user file/directory creation */
		inode->i_op = &ctrlfs_dir_inode_operations;
		inode->i_fop = &simple_dir_operations;
		inc_nlink(inode);
		break;
	case CAS_CTRL_TYPE_TAP_DIR:
		inode->i_op = &tapfs_dir_inode_operations;
		inode->i_fop = &simple_dir_operations;
		inc_nlink(inode);
		break;
	case CAS_CTRL_TYPE_TAP_FILE:
		inode->i_op = &tapfs_file_inode_operations;
		inode->i_fop = &tapfs_file_operations;
		break;
	default:
		break;
	}
	unlock_new_inode(inode);
	return inode;
}

void cas_ctrl_destroy_inode(struct inode *inode)
{
}

static int ctrl_mknod(struct inode *dir, struct dentry *dentry, umode_t mode,
		      enum cas_ctrl_type type)
{
	int error = 0;
	struct inode *inode = cas_ctrl_iget(dir->i_sb, dir, mode, type);

	if (inode) {
		casfs_d_instantiate_persistent(dentry, inode);
		casfs_inode_set_mtime_to_ts(inode,
					    casfs_inode_set_ctime_current(dir));
	} else {
		error = -ENOMEM;
	}
	return error;
}
/* Create a directory based on type */
int ctrl_mkdir(struct inode *dir, struct dentry *dentry,
	       enum cas_ctrl_type type)
{
	int retval = ctrl_mknod(dir, dentry, 0755 | S_IFDIR, type);

	if (!retval)
		inc_nlink(dir);
	return retval;
}

/* Create a file based on type */
int ctrl_create(struct inode *dir, struct dentry *dentry,
		enum cas_ctrl_type type)
{
	return ctrl_mknod(dir, dentry, 0644 | S_IFREG, type);
}
