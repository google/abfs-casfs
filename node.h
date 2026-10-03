/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google LLC
 */

#ifndef _NODE_H_
#define _NODE_H_

#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/xattr.h>
#include <linux/timer.h>
#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/ioctl.h>
#include "blob.h"
#include "fs.h"
#include "hash.h"
#include "cas_ctrl.h"
#include "compat.h"

/*
 * casfs ioctl ABI. Mirrored by api/ioctl/dirscan.go; keep the layouts in sync.
 */
#ifndef CASFS_HASH_LEN
#define CASFS_HASH_LEN 20
#endif

#define CASFS_TYPE_REG 0
#define CASFS_TYPE_REG_EXEC 1
#define CASFS_TYPE_DIR 2
#define CASFS_TYPE_COMMIT 3
#define CASFS_TYPE_SYMLINK 4
#define CASFS_TYPE_FIFO 5
#define CASFS_TYPE_SOCKET 6
#define CASFS_TYPE_DEEPTREE 7
#define CASFS_TYPE_GIT_OBJECTS_DIR 8
#define CASFS_TYPE_GIT_OBJECT_DIR 9
#define CASFS_TYPE_CTRL 10

/* Node status flags */
#define CASFS_FLAG_LOADED (1 << 0)
#define CASFS_FLAG_EXCLUDED (1 << 1)
#define CASFS_FLAG_DEBUG (1 << 2)
#define CASFS_FLAG_HAS_ARI (1 << 3)

struct casfs_dir_entry {
	__u32 ctime_sec;
	__u32 ctime_nsec;
	__u32 mtime_sec;
	__u32 mtime_nsec;
	__u64 size;
	__u64 ino;
	__u32 dev;
	__u32 mode;
	__u32 uid;
	__u32 gid;
	__s32 dirtiness;
	__u8 type;
	__u8 backend_type;
	__u16 flags;
	__u16 name_len;
	__u16 ari_len;
	__u8 sha[CASFS_HASH_LEN];
	__u8 commit_sha[CASFS_HASH_LEN];
	__u8 pad[12];
	__u8 payload[]; /* name (name_len + 1) followed by ari (ari_len + 1) */
};

struct casfs_read_dir_args {
	__u64 buf_ptr; /* Pointer to user-space buffer */
	__u64 buf_size; /* Total size of user-space buffer */
	__u64 bytes_written; /* Output: Total bytes written */
	__u32 entries_count; /* Output: Number of entries copied */
	__u32 has_more; /* Output: 1 if more entries remain */
	__u64 cookie; /* In/Out resume index */
};

#define CASFS_IOCTL_MAGIC 'C'
#define CASFS_IOC_TAP_FINISHED _IO(CASFS_IOCTL_MAGIC, 1)
#define CASFS_IOC_READ_DIR_ENTRIES \
	_IOWR(CASFS_IOCTL_MAGIC, 2, struct casfs_read_dir_args)
#define IOCTL_TAP_CMD_FINISHED CASFS_IOC_TAP_FINISHED
#define CASFS_IOC_TAP_FINISHED_OLD _IO('T', 1)

#define CAS_XATTR_HASH "hash"
#define CAS_XATTR_BIN_HASH "bin_hash"
#define CAS_XATTR_HASH_SIZE "hash_size"
#define CAS_XATTR_HASH_STAT "hash_stat"
#define CAS_XATTR_DIR_LIST_HASH "dir_list_hash"
#define CAS_XATTR_DEEPTREE "deeptree"
#define CAS_XATTR_LOADED "loaded"
#define CAS_XATTR_DEBUG "debug"
#define CAS_XATTR_DIRTY "dirty"
#define CAS_XATTR_CTRL "ctrl"
#define CAS_XATTR_TYPE "type"
#define CAS_XATTR_EXCLUDE "exclude"
#define CAS_XATTR_STATUS "fs_status"
#define CAS_XATTR_ARI_SIZE "ari_size"
#define CAS_XATTR_BACKEND_STAT "backend_stat"

#define CAS_MAX_ARI_LEN 4096

enum cas_object_type {
	CAS_TYPE_REG = CASFS_TYPE_REG,
	CAS_TYPE_REG_EXEC = CASFS_TYPE_REG_EXEC,
	CAS_TYPE_DIR = CASFS_TYPE_DIR,
	CAS_TYPE_COMMIT = CASFS_TYPE_COMMIT,
	CAS_TYPE_SYMLINK = CASFS_TYPE_SYMLINK,
	CAS_TYPE_FIFO = CASFS_TYPE_FIFO,
	CAS_TYPE_SOCKET = CASFS_TYPE_SOCKET,
	CAS_TYPE_DEEPTREE = CASFS_TYPE_DEEPTREE,
	CAS_TYPE_GIT_OBJECTS_DIR = CASFS_TYPE_GIT_OBJECTS_DIR,
	CAS_TYPE_GIT_OBJECT_DIR = CASFS_TYPE_GIT_OBJECT_DIR,
	CAS_TYPE_CTRL = CASFS_TYPE_CTRL,
};

static inline bool cas_type_is_dir(enum cas_object_type type)
{
	return type == CAS_TYPE_DIR || type == CAS_TYPE_COMMIT ||
	       type == CAS_TYPE_GIT_OBJECTS_DIR ||
	       type == CAS_TYPE_GIT_OBJECT_DIR;
}

static inline bool cas_type_is_reg_file(enum cas_object_type type)
{
	return type == CAS_TYPE_REG || type == CAS_TYPE_REG_EXEC;
}

static inline bool cas_type_is_symlink(enum cas_object_type type)
{
	return type == CAS_TYPE_SYMLINK;
}

#define CAS_ENT_MODE_REG 0100644
#define CAS_ENT_MODE_REG_EXEC 0100755
#define CAS_ENT_MODE_DIR 040000
#define CAS_ENT_MODE_COMMIT 0160000
#define CAS_ENT_MODE_SYMLINK 0120000
#define CAS_ENT_MODE_FIFO 0140000
#define CAS_ENT_MODE_SOCKET 0170000
#define CAS_ENT_MODE_DEEPTREE 0210000

struct cas_node {
	struct inode inode;
	enum cas_object_type type;
	struct mutex lock;
	union {
		struct { /* cas node */
			seqlock_t hash_lock; /* Sharing for both hashes */
			struct cas_hash hash; /* hash */
			struct cas_hash commit_hash; /* Commit hash*/
			bool debug;
			bool is_excluded; /* Excluded from hash calc */
			bool is_loaded;
			u8 backend_type;
			atomic_t dirtiness;
		};
		struct cas_ctrl_node cas_ctrl_node;
	};
	union {
		struct { /* directory */
			struct rb_root entries;
			/*
			 * dirtiness represents the number of changes in dir or
			 * its descendents after the last hash generation.
			 * Therefore, `dirtiness == 0` means the hash is still valid.
			 */
			bool valid_dir_list_hash;
			struct cas_hash dir_list_hash;
		};
		struct { /* regular file */
			struct cas_blob blob;
			const char *ari;
		};

		struct { /* symlink */
			/* Store link name within cas_node so VFS always calls into casfs for links*/
			char *link_i;
		};
	};
};

extern const struct inode_operations cas_dir_inode_operations;
extern const struct inode_operations cas_file_inode_operations;
extern const struct file_operations cas_file_operations;

static inline struct cas_node *INODE_TO_CAS(struct inode *inode)
{
	return container_of(inode, struct cas_node, inode);
}

static inline struct inode *CAS_TO_INODE(struct cas_node *node)
{
	return &node->inode;
}

static inline void cas_lock_node(struct inode *inode)
{
	mutex_lock(&INODE_TO_CAS(inode)->lock);
}

static inline void cas_unlock_node(struct inode *inode)
{
	mutex_unlock(&INODE_TO_CAS(inode)->lock);
}

static inline void cas_set_node_hash(struct cas_node *node,
				     const struct cas_hash *hash, loff_t size)
{
	write_seqlock(&node->hash_lock);
	node->hash = *hash;
	i_size_write(CAS_TO_INODE(node), size); /* Do we need lock? */
	write_sequnlock(&node->hash_lock);
}

static inline void cas_get_node_hash(const struct cas_node *node,
				     struct cas_hash *hash, loff_t *size)
{
	unsigned int seq;

	do {
		seq = read_seqbegin(&node->hash_lock);
		*hash = node->hash;
		*size = i_size_read(&node->inode);
	} while (read_seqretry(&node->hash_lock, seq));
}
static inline void cas_get_node_bin_hash(const struct cas_node *node, char *buf,
					 uint64_t *size)
{
	unsigned int seq;

	do {
		seq = read_seqbegin(&node->hash_lock);
		memcpy(buf, &node->hash.bytes, CASFS_HASH_LEN);
		*size = i_size_read(&node->inode);
	} while (read_seqretry(&node->hash_lock, seq));
}
static inline void cas_set_node_commit_hash(struct cas_node *node,
					    const struct cas_hash *hash)
{
	write_seqlock(&node->hash_lock);
	node->commit_hash = *hash;
	write_sequnlock(&node->hash_lock);
}

static inline void cas_get_node_commit_hash(const struct cas_node *node,
					    struct cas_hash *hash)
{
	unsigned int seq;

	do {
		seq = read_seqbegin(&node->hash_lock);
		*hash = node->commit_hash;
	} while (read_seqretry(&node->hash_lock, seq));
}

static inline bool cas_empty_or_not_loaded(struct dentry *dentry)
{
	return simple_empty(dentry);
}

extern struct inode *cas_new_inode_locked(struct super_block *sb);

extern struct inode *cas_new_node(struct super_block *sb,
				  enum cas_object_type type,
				  const struct cas_hash *hash, uint64_t size,
				  umode_t mode);

static inline bool cas_node_is_dirty(struct cas_node *node)
{
	return atomic_read(&node->dirtiness) != 0;
}
/* file.c */
extern const struct file_operations cas_file_operations;
extern int cas_force_file_hash(struct dentry *dentry,
			       const struct cas_hash *hash, uint64_t size);
extern int cas_force_file_ari(struct dentry *dentry, const char *ari,
			      int ari_len, uint64_t size);
extern void cas_free_file(struct cas_node *node);
extern int cas_file_snapshot(struct cas_node *node);

extern int cas_open(struct inode *inode, struct file *file);
extern int cas_release(struct inode *inode, struct file *file);
extern loff_t cas_llseek(struct file *file, loff_t offset, int whence);
extern ssize_t cas_read_iter(struct kiocb *iocb, struct iov_iter *iter);
extern ssize_t cas_write_iter(struct kiocb *iocb, struct iov_iter *iter);
extern int cas_fsync(struct file *file, loff_t start, loff_t end, int datasync);
extern int cas_mmap(struct file *file, struct vm_area_struct *vma);
extern long cas_fallocate(struct file *file, int mode, loff_t offset,
			  loff_t len);
extern int cas_fadvise(struct file *file, loff_t offset, loff_t len,
		       int advice);
extern int cas_flush(struct file *file, fl_owner_t id);
extern ssize_t cas_splice_read(struct file *in, loff_t *ppos,
			       struct pipe_inode_info *pipe, size_t len,
			       unsigned int flags);
extern ssize_t cas_splice_write(struct pipe_inode_info *pipe, struct file *out,
				loff_t *ppos, size_t len, unsigned int flags);
extern void cas_file_scratch_setup_external(struct cas_node *node,
					    const struct path *path,
					    loff_t size);
/* dir.c */
extern const struct file_operations cas_dir_operations;
extern int cas_dir_ensure_loaded(struct dentry *dentry);
struct dentry *cas_mkdir_internal(struct inode *dir, struct dentry *dentry,
				  enum cas_object_type type, umode_t mode);
extern void cas_free_dir(struct cas_node *node);
extern void cas_make_dirty(struct dentry *dentry);
extern void cas_dir_changed(struct dentry *dentry);
extern int cas_force_dir_hash(struct dentry *dentry,
			      const struct cas_hash *hash);
extern int cas_dir_set_deeptree(struct dentry *dentry,
				const struct cas_hash *hash);
extern int cas_get_mode_from_type(enum cas_object_type type, int *mode);
extern int cas_get_type_from_mode(int mode, enum cas_object_type *type);
extern int cas_get_dir_list_hash(struct cas_node *node, struct cas_hash *hash);
extern int cas_dir_snapshot(struct dentry *dentry);
extern int cas_dir_snapshot_list(struct cas_node *node);

extern struct dentry *cas_lookup(struct inode *dir, struct dentry *dentry,
				 unsigned int flags);
extern struct dentry *cas_mkdir(struct mnt_idmap_type *mnt_userns,
				struct inode *dir, struct dentry *dentry,
				umode_t mode);
extern int cas_symlink(struct mnt_idmap_type *mnt_userns, struct inode *dir,
		       struct dentry *dentry, const char *link);
extern int cas_unlink(struct inode *dir, struct dentry *dentry);
extern int cas_rmdir(struct inode *dir, struct dentry *dentry);
extern int cas_rename(struct mnt_idmap_type *mnt_userns, struct inode *old_dir,
		      struct dentry *old, struct inode *new_dir,
		      struct dentry *new, unsigned int flags);
extern int cas_link(struct dentry *source, struct inode *dir,
		    struct dentry *dentry);
extern int cas_create(struct mnt_idmap_type *mnt_userns, struct inode *dir,
		      struct dentry *dentry, umode_t mode, bool excl);
extern int cas_mknod(struct mnt_idmap_type *mnt_userns, struct inode *dir,
		     struct dentry *dentry, umode_t mode, dev_t rdev);
extern int cas_dir_add_child(struct inode *dir, struct dentry *dentry,
			     struct inode *new_inode);

extern long cas_dir_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg);
#ifdef CONFIG_COMPAT
extern long cas_dir_compat_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg);
#endif

extern int casfs_revalidate(struct dentry *dentry, unsigned int flags);
extern const struct dentry_operations casfs_dentry_operations;

/* node.c */
extern void cas_destroy_inode(struct inode *inode);
int cas_setattr(struct mnt_idmap_type *mnt_userns, struct dentry *dentry,
		struct iattr *attr);
int cas_getattr(struct mnt_idmap_type *mnt_userns, const struct path *path,
		struct kstat *stat, u32 request_mask, unsigned int flags);
extern int cas_xattr_get(const struct xattr_handler *handler,
			 struct dentry *dentry, struct inode *inode,
			 const char *name, void *buffer, size_t size);
extern int cas_xattr_set(const struct xattr_handler *handler,
			 struct mnt_idmap_type *mnt_userns,
			 struct dentry *dentry, struct inode *inode,
			 const char *name, const void *buffer, size_t size,
			 int flags);
extern int cas_snapshot_node(struct cas_node *node, struct cas_hash *hash,
			     uint64_t *size);
extern int cas_load_symlink(struct inode *inode);
extern int cas_load_node(struct dentry *dentry, struct inode *inode);

#endif /*_NODE_H_*/
