// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 */
#include <linux/namei.h>

#include "fs.h"
#include "node.h"
#include "cas_ctrl.h"
#include "casfs.h"
#include "compat.h"
#include "compat_inode.h"
#include "dir_rb_tree.h"
#include "ctrl/util.h"
#include "ctrl/inode.h"
#include "ctrl/git/inode.h"

/* Sub module directories */
#define CASFS_TAP_DIR "tap"

struct cas_ctrl *cas_ctrl_create(struct dentry *debugfs_root, int instance)
{
	struct cas_ctrl *ctrl = kzalloc(sizeof(struct cas_ctrl), GFP_KERNEL);

	if (unlikely(!ctrl))
		return NULL;

	mutex_init(&ctrl->lock);

	ctrl->tap = tap_init(debugfs_root, instance);
	if (unlikely(!ctrl->tap))
		goto out_destroy;

	ctrl->git = git_init();
	if (unlikely(!ctrl->git))
		goto out_destroy;

	return ctrl;

out_destroy:
	cas_ctrl_destroy(ctrl);

	return NULL;
}

void cas_ctrl_destroy(struct cas_ctrl *ctrl)
{
	if (ctrl->tap)
		tap_destroy(ctrl->tap);

	if (ctrl->git)
		git_destroy(ctrl->git);

	kfree(ctrl);
}

static int ctrl_dir_add_helper(struct dentry *parent, const char *name,
			       struct dentry **out, enum cas_ctrl_type type)
{
	int rc = 0;
	struct dentry *dentry = casfs_lookup_one_unlocked(casfs_nop_idmap, name,
							  parent, strlen(name));

	if (IS_ERR_OR_NULL(dentry)) {
		rc = dentry ? PTR_ERR(dentry) : -ENOMEM;
		goto out_err;
	}

	rc = ctrl_mkdir(d_inode(parent), dentry, type);
	if (unlikely(rc != 0))
		goto out_dput;

	if (out)
		*out = dentry;
	else
		dput(dentry);

	return rc;

out_dput:
	dput(dentry);
out_err:
	return rc;
}

static int cas_ctrl_dir_dentry(struct dentry *dir, struct cas_node *node,
			       const char *name, size_t size,
			       struct dentry **out_dir)
{
	int rc = 0;
	struct dentry *new_dir = NULL;

	if (node->type != CAS_TYPE_DIR) {
		rc = -ENOTDIR;
		goto out_err;
	}

	new_dir = casfs_lookup_one(casfs_nop_idmap, name, dir, size);
	if (IS_ERR_OR_NULL(new_dir)) {
		rc = new_dir ? PTR_ERR(new_dir) : -ENOMEM;
		goto out_err;
	}
	if (!d_is_negative(new_dir)) {
		rc = -EEXIST;
		goto out_dput;
	}

	*out_dir = new_dir;
	return 0;

out_dput:
	dput(new_dir);

out_err:
	return rc;
}

static int cas_ctrl_mnt(struct dentry *dir, struct cas_node *node,
			const char *name, size_t size)
{
	int rc = 0;
	struct dentry *ctrl_dir = NULL;

	rc = cas_ctrl_dir_dentry(dir, node, name, size, &ctrl_dir);
	if (unlikely(rc != 0))
		return rc;

	rc = ctrl_mkdir(d_inode(dir), ctrl_dir, CAS_CTRL_TYPE_DIR);
	if (unlikely(rc != 0))
		goto out_dput;

	rc = ctrl_dir_add_helper(ctrl_dir, CASFS_TAP_DIR, NULL,
				 CAS_CTRL_TYPE_TAP_DIR);

out_dput:
	dput(ctrl_dir); /* Drops lookup reference */
	return rc;
}

int cas_ctrl_xattr_set(struct dentry *dentry, struct inode *inode,
		       const char *name, const void *buffer, size_t size)
{
	int i = 0;
	const char *input = (const char *)buffer;
	const char *dir = 0;
	struct cas_node *node = INODE_TO_CAS(inode);

	/* Input is in the form of type:dir_name parse here */
	for (; i < size; i++) {
		if (input[i] == ':')
			break;
	}

	/* Check that delim was found and at least one character follows it */
	if (i >= (size - 1))
		return -EINVAL;

	dir = &input[i + 1];
	size -= (i + 1);

	if (strncmp(input, CAS_CTRL_TYPE_CONTROL_NAME, i) == 0)
		return cas_ctrl_mnt(dentry, node, dir, size);

	if (strncmp(input, CAS_CTRL_TYPE_GIT_OBJ_NAME, i) == 0)
		return create_git_object_dir_locked(dentry, dir, size);

	return -EINVAL;
}
