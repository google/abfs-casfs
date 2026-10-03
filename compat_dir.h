/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Google LLC
 */

#ifndef _CASFS_COMPAT_DIR_H_
#define _CASFS_COMPAT_DIR_H_

#include <linux/fs.h>
#include <linux/version.h>

#include "compat.h"
#include "node.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
#define HAS_VFS_DELEGATED_INODE 1
#else
#define HAS_VFS_DELEGATED_INODE 0
#endif

// git describe --contains da549bdd15c29
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)
#define cas_list_for_each_child(child, dentry) \
	list_for_each_entry(child, &dentry->d_subdirs, d_child)
#else
#define cas_list_for_each_child(child, dentry) \
	hlist_for_each_entry(child, &dentry->d_children, d_sib)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
#define WRAP_DIR_ITER(x)                                                  \
	static int shared_##x(struct file *file, struct dir_context *ctx) \
	{                                                                 \
		return x(file, ctx);                                      \
	}
#endif

static inline int casfs_vfs_mkdir(struct mnt_idmap_type *idmap,
				  struct inode *dir, struct dentry *dentry,
				  umode_t mode)
{
#if HAS_VFS_DELEGATED_INODE
	struct dentry *res;

	res = vfs_mkdir(idmap, dir, dentry, mode, NULL);
	if (IS_ERR(res))
		return PTR_ERR(res);

	return 0;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	struct dentry *res;

	res = vfs_mkdir(idmap, dir, dentry, mode);
	if (IS_ERR(res))
		return PTR_ERR(res);

	return 0;
#else
	return vfs_mkdir(idmap, dir, dentry, mode);
#endif
}

static inline int casfs_vfs_create(struct mnt_idmap_type *idmap,
				   struct dentry *parent_dentry,
				   struct dentry *dentry, umode_t mode,
				   bool want_excl)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
	(void)parent_dentry;
	(void)want_excl;
	return vfs_create(idmap, dentry, mode, NULL);
#else
	return vfs_create(idmap, d_inode(parent_dentry), dentry, mode,
			  want_excl);
#endif
}

static inline int casfs_vfs_mknod(struct mnt_idmap_type *idmap,
				  struct inode *dir, struct dentry *dentry,
				  umode_t mode, dev_t dev)
{
#if HAS_VFS_DELEGATED_INODE
	return vfs_mknod(idmap, dir, dentry, mode, dev, NULL);
#else
	return vfs_mknod(idmap, dir, dentry, mode, dev);
#endif
}

static inline int casfs_vfs_symlink(struct mnt_idmap_type *idmap,
				    struct inode *dir, struct dentry *dentry,
				    const char *oldname)
{
#if HAS_VFS_DELEGATED_INODE
	return vfs_symlink(idmap, dir, dentry, oldname, NULL);
#else
	return vfs_symlink(idmap, dir, dentry, oldname);
#endif
}

static inline int casfs_vfs_rmdir(struct mnt_idmap_type *idmap,
				  struct dentry *parent_dentry,
				  struct dentry *dentry)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
	(void)parent_dentry;
	return vfs_rmdir(idmap, d_inode(parent_dentry), dentry, NULL);
#else
	return vfs_rmdir(idmap, d_inode(parent_dentry), dentry);
#endif
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 15, 0)
static inline int cas_mkdir_op(struct mnt_idmap_type *idmap, struct inode *dir,
			       struct dentry *dentry, umode_t mode)
{
	struct dentry *res = cas_mkdir(idmap, dir, dentry, mode);

	return PTR_ERR_OR_ZERO(res);
}
#else
static inline struct dentry *cas_mkdir_op(struct mnt_idmap_type *idmap,
					  struct inode *dir,
					  struct dentry *dentry, umode_t mode)
{
	return cas_mkdir(idmap, dir, dentry, mode);
}
#endif

static inline void casfs_set_default_d_op(struct super_block *sb,
					  const struct dentry_operations *ops)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
	set_default_d_op(sb, ops);
#else
	sb->s_d_op = ops;
#endif
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 15, 0)
static inline int casfs_revalidate_op(struct dentry *dentry, unsigned int flags)
{
	return casfs_revalidate(dentry, flags);
}
#else
static inline int casfs_revalidate_op(struct inode *dir,
				      const struct qstr *name,
				      struct dentry *dentry, unsigned int flags)
{
	return casfs_revalidate(dentry, flags);
}
#endif

#endif /* _CASFS_COMPAT_DIR_H_ */
