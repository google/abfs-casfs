/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google LLC
 */

#ifndef _CAS_CTRL_H
#define _CAS_CTRL_H

#include <linux/namei.h>
#include <linux/mutex.h>

/* Control modules */
#include "provider.h"
#include "ctrl/tap/ctrl.h"
#include "ctrl/git/ctrl.h"

/* Control structure */
struct cas_ctrl {
	struct mutex lock;
	struct tap_ctrl *tap;
	struct git_ctrl *git;
};

/* Allow non-cas directories to be created via
 * CAS_XATTR_CTRL in the form <type:dir>
 */
#define CAS_CTRL_TYPE_CONTROL_NAME "ctrl"
#define CAS_CTRL_TYPE_GIT_OBJ_NAME "gitobj"

enum cas_ctrl_type {
	CAS_CTRL_TYPE_DIR,
	CAS_CTRL_TYPE_CONTROL,
	CAS_CTRL_TYPE_TAP_DIR,
	CAS_CTRL_TYPE_TAP_FILE,
};

/* Control inode data */
struct cas_ctrl_node {
	enum cas_ctrl_type type;
};
/* FS init/destroy for control */
extern struct cas_ctrl *cas_ctrl_create(struct dentry *debugfs_root,
					int instance);

extern void cas_ctrl_destroy(struct cas_ctrl *ctrl);

extern void cas_ctrl_destroy_inode(struct inode *inode);

extern int cas_ctrl_xattr_set(struct dentry *dentry, struct inode *inode,
			      const char *name, const void *buffer,
			      size_t size);
extern int cas_ctrl_xattr_create_index(struct dentry *dentry,
				       struct inode *inode, const char *name,
				       const void *buffer, size_t size);

#endif /*_CAS_CTRL_H*/
