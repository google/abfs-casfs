/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Google LLC
 */

#ifndef _INC_GIT_COMPAT_DIR_H_
#define _INC_GIT_COMPAT_DIR_H_

#include <linux/fs.h>
#include <linux/version.h>
#include "../../compat.h"

struct dentry *__gitfs_objects_dir_mkdir(struct mnt_idmap_type *mnt_userns,
					 struct inode *dir,
					 struct dentry *dentry, umode_t mode);

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 15, 0)
static inline int gitfs_objects_dir_mkdir(struct mnt_idmap_type *idmap,
					  struct inode *dir,
					  struct dentry *dentry, umode_t mode)
{
	struct dentry *res =
		__gitfs_objects_dir_mkdir(idmap, dir, dentry, mode);
	return PTR_ERR_OR_ZERO(res);
}
#else
static inline struct dentry *
gitfs_objects_dir_mkdir(struct mnt_idmap_type *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode)
{
	return __gitfs_objects_dir_mkdir(idmap, dir, dentry, mode);
}
#endif

#endif /* _INC_GIT_COMPAT_DIR_H_ */
