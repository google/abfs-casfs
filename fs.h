/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google LLC
 */

#ifndef _FS_H_
#define _FS_H_

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/idr.h>
#include "cache.h"
#include "provider.h"
#include "cas_ctrl.h"
#include "sysfs.h"

#define XATTR_CASFS_PREFIX "casfs."
#define XATTR_USER_CASFS_PREFIX "user.casfs_"
#define DEFAULT_MAX_TAP_ENTRIES (4194304*2)
extern int casfs_max_mounts;
extern int casfs_max_git_pools;
extern int casfs_max_tap_entries;
extern atomic_t casfs_mount_slots;
extern struct ida casfs_instance_ida;
extern struct file_system_type cas_fs_type;

/* casfs_mount_opts - mount options for casfs
 */
struct casfs_mount_opts {
	bool is_local;
	int fetch_timeout;
	char *cache_dir_str;
	char *cache_scratch;
	char *cache_socket;
	bool debug;
};

struct cas_fs {
	struct super_block *sb;
	char *cache_dir_str;
	char *cache_socket_str;
	char *init_hash_hex;
	struct cas_cache cache;
	bool is_local;
	struct cas_provider provider;
	struct inode *cache_root_inode;
	struct dentry *debugfs_root;
	struct casfs_sysfs sysfs;

	int fetch_timeout;
	struct timespec64 mount_time;
	atomic64_t next_ino;
	atomic_t inodes_in_use;
	struct cas_ctrl *ctrl;
	bool ready;
	int instance_ida;
	struct kmem_cache *node_cache;
	char cache_name[32];
};

static inline int cas_fs_check(struct cas_fs *fs)
{
	if (fs->is_local)
		return 0;

	return cas_provider_check(&fs->provider);
}
static inline struct cas_fs *SB_TO_FS(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct super_block *FS_TO_SB(struct cas_fs *fs)
{
	return fs->sb;
}
static inline long long get_next_casfs_ino(struct cas_fs *fs)
{
	return atomic64_inc_return(&fs->next_ino);
}
extern struct super_operations cas_super_ops;

extern int cas_fill_super(struct super_block *sb, struct fs_context *fc);

extern struct inode *cas_alloc_inode(struct super_block *sb);

extern void cas_put_super(struct super_block *sb);

static inline struct cas_store *cas_fs_object_store(struct cas_fs *fs)
{
	return &fs->cache.objects;
}

extern int cas_fs_new_blob(struct cas_fs *fs, const struct cas_hash *hash,
			   const void *buf, size_t len);

extern void cas_fs_root_dir_change(struct cas_fs *fs,
				   const struct cas_hash *hash);

#endif /*_FS_H_*/
