/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Google LLC
 */
#ifndef _CASFS_COMPAT_INODE_H_
#define _CASFS_COMPAT_INODE_H_

#include <linux/fs.h>
#include <linux/version.h>
#include <linux/time64.h>
#include "compat.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
#define casfs_inode_is_new(inode) (inode_state_read_once(inode) & I_NEW)
#else
#define casfs_inode_is_new(inode) ((inode)->i_state & I_NEW)
#endif

static inline struct timespec64
casfs_inode_set_atime_to_ts(struct inode *inode, struct timespec64 ts)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
	return inode_set_atime_to_ts(inode, ts);
#else
	inode->i_atime = ts;
	return ts;
#endif
}

static inline struct timespec64
casfs_inode_set_mtime_to_ts(struct inode *inode, struct timespec64 ts)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
	return inode_set_mtime_to_ts(inode, ts);
#else
	inode->i_mtime = ts;
	return ts;
#endif
}

static inline struct timespec64 casfs_inode_get_atime(const struct inode *inode)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
	return inode_get_atime(inode);
#else
	return inode->i_atime;
#endif
}

static inline struct timespec64 casfs_inode_get_mtime(const struct inode *inode)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
	return inode_get_mtime(inode);
#else
	return inode->i_mtime;
#endif
}

static inline struct timespec64
casfs_inode_set_ctime_to_ts(struct inode *inode, struct timespec64 ts)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0)
	return inode_set_ctime_to_ts(inode, ts);
#else
	inode->i_ctime = ts;
	return ts;
#endif
}

static inline struct timespec64
casfs_inode_set_ctime_current(struct inode *inode)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0)
	return inode_set_ctime_current(inode);
#else
	struct timespec64 now = current_time(inode);
	inode->i_ctime = now;
	return now;
#endif
}

static inline struct timespec64 casfs_inode_get_ctime(const struct inode *inode)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0)
	return inode_get_ctime(inode);
#else
	return inode->i_ctime;
#endif
}

static inline int casfs_notify_change(struct mnt_idmap_type *idmap,
				      struct dentry *dentry, struct iattr *attr)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
	return notify_change(dentry, attr, NULL);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
	return notify_change(&init_user_ns, dentry, attr, NULL);
#else
	return notify_change(idmap, dentry, attr, NULL);
#endif
}

static inline int casfs_file_permission(struct file *file, struct inode *inode,
					int mask)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	return inode_permission(file_mnt_idmap(file), inode, mask);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
	return inode_permission(file_mnt_user_ns(file), inode, mask);
#else
	return inode_permission(inode, mask);
#endif
}

static inline void cas_touch_mtime_ctime_ts(struct inode *inode,
					    struct timespec64 ts)
{
	casfs_inode_set_mtime_to_ts(inode, ts);
	casfs_inode_set_ctime_to_ts(inode, ts);
	mark_inode_dirty(inode);
}

static inline void cas_touch_mtime_ctime(struct inode *inode)
{
	cas_touch_mtime_ctime_ts(inode, current_time(inode));
}

#endif /* _CASFS_COMPAT_INODE_H_ */
