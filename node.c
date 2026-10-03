// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 */

#include <uapi/linux/stat.h>
#include <linux/xattr.h>
#include <linux/namei.h>
#include <linux/parser.h>
#include "compat.h"
#include "compat_inode.h"
#include "blob.h"
#include "node.h"
#include "fs.h"
#include "dir_rb_tree.h"
#include "cas_tap.h"
#include "cas_ctrl.h"
#include "ctrl/git/inode.h"

int cas_load_symlink(struct inode *inode)
{
	int err;
	struct cas_fs *fs = SB_TO_FS(inode->i_sb);
	struct cas_node *node = INODE_TO_CAS(inode);
	ssize_t size;
	char *buf, *link;

	err = cas_provider_get_blob_bytes(&fs->provider, &node->hash,
					  (void **)&buf, &size);
	if (unlikely(err))
		return err;

	link = kstrndup(buf, size, GFP_KERNEL);
	kfree(buf);
	if (unlikely(!link))
		return -ENOMEM;

	node->is_loaded = true;
	node->link_i = link;
	i_size_write(inode, strlen(node->link_i));

	return 0;
}
static const char *cas_get_link(struct dentry *dentry, struct inode *inode,
				struct delayed_call *done)
{
	int err;
	struct cas_node *node = INODE_TO_CAS(inode);

	pr_debug("%s(%llu,%20phN)\n", __func__,
		 (unsigned long long)inode->i_ino, &node->hash);

	/* Reject the rcu-walk, it will fallback to ref-walk.
	 * Need to do this for all cases (even when link is known)
	 * to get a valid dentry for tapping
	 */
	if (!dentry)
		return ERR_PTR(-ECHILD);

	cas_lock_node(inode);
	if (node->link_i) {
		cas_tap_symlink_read(dentry);
		cas_unlock_node(inode);
		return node->link_i;
	}

	err = cas_load_symlink(inode);
	cas_tap_symlink_read(dentry);
	cas_unlock_node(inode);

	return err ? ERR_PTR(err) : node->link_i;
}

/* Snapshot the input node and get the representative hash*/

int cas_snapshot_node(struct cas_node *node, struct cas_hash *hash,
		      uint64_t *size)
{
	int ret = 0;

	if (node->is_loaded) {
		switch (node->type) {
		case CAS_TYPE_REG:
		case CAS_TYPE_REG_EXEC:
			ret = cas_file_snapshot(node);
			break;
		default:
			break;
		}
	}

	if (ret == 0)
		cas_get_node_hash(node, hash, size);

	return ret;
}

int cas_getattr(struct mnt_idmap_type *mnt_userns, const struct path *path,
		struct kstat *stat, u32 request_mask, unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);

	pr_debug("%s %llu\n", __func__, (unsigned long long)inode->i_ino);

	stat->dev = inode->i_sb->s_dev;
	stat->rdev = inode->i_rdev;
	stat->ino = inode->i_ino;
	stat->mode = inode->i_mode;
	stat->uid = inode->i_uid;
	stat->gid = inode->i_gid;
	stat->nlink = inode->i_nlink;
	stat->atime = casfs_inode_get_atime(inode);
	stat->mtime = casfs_inode_get_mtime(inode);
	stat->ctime = casfs_inode_get_ctime(inode);
	cas_lock_node(inode);
	stat->size = i_size_read(inode);
	cas_tap_getattr(path->dentry);
	cas_unlock_node(inode);
	stat->blksize = path->dentry->d_sb->s_blocksize;
	stat->blocks = (stat->size + stat->blksize - 1) / stat->blksize;

	return 0;
}

static const struct inode_operations cas_symlink_inode_operations = {
	.get_link = cas_get_link,
	.getattr = cas_getattr,
};

static inline struct timespec64 cas_get_time(struct cas_node *node)
{
	struct cas_fs *fs = SB_TO_FS(CAS_TO_INODE(node)->i_sb);

	return fs->mount_time;
}

struct inode *cas_new_inode_locked(struct super_block *sb)
{
	struct inode *inode;
	struct cas_fs *fs = SB_TO_FS(sb);
	struct timespec64 ts;

	inode = iget_locked(sb, get_next_casfs_ino(fs));
	if (unlikely(!inode))
		return NULL;

	pr_debug("got an inode with %llu\n", (unsigned long long)inode->i_ino);
	WARN_ON(!casfs_inode_is_new(inode));

	ts = casfs_inode_set_ctime_current(inode);
	casfs_inode_set_mtime_to_ts(inode, ts);
	casfs_inode_set_atime_to_ts(inode, ts);
	inode->i_uid = fs->cache_root_inode->i_uid;
	inode->i_gid = fs->cache_root_inode->i_gid;
	inode->i_link = NULL;

	return inode;
}

struct inode *cas_new_node(struct super_block *sb, enum cas_object_type type,
			   const struct cas_hash *hash, uint64_t size,
			   umode_t mode)
{
	struct cas_node *node;
	struct inode *inode;

	/* Only support cas-type nodes */
	if (type == CAS_TYPE_CTRL)
		return ERR_PTR(-EINVAL);

	inode = cas_new_inode_locked(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	node = INODE_TO_CAS(inode);
	node->is_loaded = false;
	node->debug = false;
	node->type = type;
	node->is_excluded = false;
	seqlock_init(&node->hash_lock);
	mutex_init(&node->lock);
	cas_set_node_hash(node, hash, size);
	cas_set_node_commit_hash(node, &cas_zero_hash);
	cas_dir_init_rb_tree(&node->entries);
	set_nlink(inode, 1);
	atomic_set(&node->dirtiness, 0);

	switch (type) {
	case CAS_TYPE_DIR:
		inode->i_op = &cas_dir_inode_operations;
		inode->i_fop = &cas_dir_operations;
		inode->i_mode = (mode == 0) ? (S_IFDIR | 0755) :
					      (S_IFDIR | mode);
		node->valid_dir_list_hash = false;
		node->backend_type = cas_type_tree;
		break;
	case CAS_TYPE_REG_EXEC:
	case CAS_TYPE_REG:
		inode->i_op = &cas_file_inode_operations;
		inode->i_fop = &cas_file_operations;
		inode->i_mode = (mode == 0) ?
					(S_IFREG |
					 (type == CAS_TYPE_REG ? 0644 : 0755)) :
					(S_IFREG | mode);
		cas_blob_init(&node->blob);
		node->backend_type = cas_type_blob;
		node->ari = NULL;
		break;
	case CAS_TYPE_SYMLINK:
		inode->i_op = &cas_symlink_inode_operations;
		inode->i_mode = (mode == 0) ? (S_IFLNK | 0777) :
					      (S_IFLNK | mode);
		node->link_i = NULL;
		node->backend_type = cas_type_blob;
		break;
	case CAS_TYPE_FIFO:

		init_special_inode(inode,
				   (mode == 0) ? (S_IFIFO | 0640) :
						 (S_IFIFO | mode),
				   WHITEOUT_DEV);
		node->is_excluded = true;
		break;
	case CAS_TYPE_SOCKET:
		init_special_inode(inode,
				   (mode == 0) ? (S_IFSOCK | 0750) :
						 (S_IFSOCK | mode),
				   WHITEOUT_DEV);
		node->is_excluded = true;
		break;
	case CAS_TYPE_COMMIT:
		/* FIXME commits (submodules) are not fully supported */
		cas_set_node_hash(node, &cas_empty_dir_hash, 0);
		cas_set_node_commit_hash(node, hash);
		inode->i_op = &cas_dir_inode_operations;
		inode->i_fop = &cas_dir_operations;
		inode->i_mode = S_IFDIR | 0755;
		atomic_set(&node->dirtiness, 0);
		node->valid_dir_list_hash = false;
		node->backend_type = cas_type_object;
		break;
	case CAS_TYPE_GIT_OBJECTS_DIR:
		inode->i_op = &gitfs_objects_dir_inode_operations;
		inode->i_fop = &cas_dir_operations;
		inode->i_mode = (mode == 0) ? (S_IFDIR | 0755) :
					      (S_IFDIR | mode);
		node->valid_dir_list_hash = false;
		node->backend_type = cas_type_tree;
		node->is_excluded = true;
		node->is_loaded = true;
		break;
	case CAS_TYPE_GIT_OBJECT_DIR:
		inode->i_op = &gitfs_object_dir_inode_operations;
		inode->i_fop = &cas_dir_operations;
		inode->i_mode = (mode == 0) ? (S_IFDIR | 0755) :
					      (S_IFDIR | mode);
		node->valid_dir_list_hash = false;
		node->backend_type = cas_type_tree;
		node->is_excluded = true;
		node->is_loaded = true;
		break;
	default:
		break;
	}

	unlock_new_inode(inode);
	return inode;
}

void cas_destroy_inode(struct inode *inode)
{
	struct cas_node *node = INODE_TO_CAS(inode);

	pr_debug("destroying inode %llu %20phN\n",
		 (unsigned long long)inode->i_ino, &node->hash);

	switch (node->type) {
	case CAS_TYPE_COMMIT:
	case CAS_TYPE_DIR:
	case CAS_TYPE_GIT_OBJECTS_DIR:
	case CAS_TYPE_GIT_OBJECT_DIR:
		cas_free_dir(node);
		break;
	case CAS_TYPE_SYMLINK:
		kfree(node->link_i);
		break;
	case CAS_TYPE_CTRL:
		cas_ctrl_destroy_inode(inode);
		break;
	case CAS_TYPE_REG_EXEC:
	case CAS_TYPE_REG:
		cas_free_file(node);
		break;
	default:
		break;
	}
}

static int cas_xattr_get_hash_common(struct dentry *dentry,
				     struct cas_node *node, const char *name,
				     char *buf, size_t size, bool binary)
{
	int err = 0;
	struct cas_hash hash;
	uint64_t hash_size;

	if (!size)
		return binary ? CASFS_HASH_LEN : CASFS_HASH_HEX_LEN + 1;

	if (binary) {
		if (unlikely(size < CASFS_HASH_LEN))
			return -ERANGE;
	} else {
		if (unlikely(size < CASFS_HASH_HEX_LEN + 1))
			return -ERANGE;
	}

	if (cas_type_is_reg_file(node->type)) {
		err = cas_file_snapshot(node);
		if (unlikely(err))
			return err;
	} else if (cas_type_is_dir(node->type)) {
		err = cas_dir_snapshot(dentry);
		if (unlikely(err))
			return err;
	}

	if (binary) {
		cas_get_node_bin_hash(node, buf, &hash_size);
	} else {
		cas_get_node_hash(node, &hash, &hash_size);
		cas_hash_fill_hex(&hash, buf);
	}

	return binary ?
		       CASFS_HASH_LEN :
		       CASFS_HASH_HEX_LEN; /* ignore the null-termination byte */
}

static int cas_xattr_get_backend_stat(struct dentry *dentry,
				      struct cas_node *node, const char *name,
				      char *buf, size_t size)
{
	int err = 0;
	size_t len = 0;
	struct cas_hash hash;
	uint64_t hash_size = 0;
	int mode = 0;
	struct inode *inode = CAS_TO_INODE(node);

	if (cas_node_is_dirty(node)) {
		if (cas_type_is_reg_file(node->type)) {
			err = cas_file_snapshot(node);
			if (unlikely(err))
				return err;
		} else if (cas_type_is_dir(node->type)) {
			err = cas_dir_snapshot(dentry);
			if (unlikely(err))
				return err;
		}
	}

	if (!size)
		buf = NULL;

	cas_lock_node(inode);

	mode = node->inode.i_mode;
	if (node->backend_type == cas_type_ari) {
		len = snprintf(buf, size, "ari:%s:%lld:%d",
			       node->ari ? node->ari : "", i_size_read(inode),
			       mode);
	} else {
		cas_get_node_hash(node, &hash, &hash_size);
		if (cas_type_is_dir(node->type))
			hash_size = 0;

		len = snprintf(buf, size, "hash:%20phN:%lld:%d", &hash,
			       hash_size, mode);
	}

	cas_unlock_node(inode);

	if (!size)
		return len + 1;

	if (unlikely(size <= len))
		return -ERANGE;

	return len;
}

static int cas_xattr_get_hash(struct dentry *dentry, struct cas_node *node,
			      const char *name, char *buf, size_t size)
{
	return cas_xattr_get_hash_common(dentry, node, name, buf, size, false);
}

static int cas_xattr_get_bin_hash(struct dentry *dentry, struct cas_node *node,
				  const char *name, char *buf, size_t size)
{
	return cas_xattr_get_hash_common(dentry, node, name, buf, size, true);
}

static int cas_xattr_set_hash(struct dentry *dentry, struct cas_node *node,
			      const char *name, const char *buf, size_t len)
{
	int err;
	struct cas_hash hash;
	struct cas_fs *fs = SB_TO_FS(CAS_TO_INODE(node)->i_sb);
	ssize_t size;

	err = cas_hash_from_hex(buf, (int)len, &hash);
	if (unlikely(err))
		return err;

	switch (node->type) {
	case CAS_TYPE_DIR:
		err = cas_force_dir_hash(dentry, &hash);
		break;
	case CAS_TYPE_REG:
	case CAS_TYPE_REG_EXEC:
		err = cas_provider_get_blob_size(&fs->provider, &hash, &size);
		if (unlikely(err))
			return err;
		err = cas_force_file_hash(dentry, &hash, size);
		break;
	default:
		err = -EOPNOTSUPP;
	}
	return err;
}

static int cas_xattr_get_hash_size(struct dentry *dentry, struct cas_node *node,
				   const char *name, char *buf, size_t size)
{
	int err = 0;
	struct cas_hash hash;
	uint64_t hash_size;
	size_t len;

	if (cas_type_is_reg_file(node->type)) {
		err = cas_file_snapshot(node);
		if (unlikely(err))
			return err;
	}
	cas_get_node_hash(node, &hash, &hash_size);

	if (!size)
		buf = NULL;

	len = snprintf(buf, size, "%20phN:%lld", &hash, hash_size);

	if (!size)
		return len + 1;

	if (unlikely(size <= len))
		return -ERANGE;

	return len;
}
static int cas_xattr_get_ari_size(struct dentry *dentry, struct cas_node *node,
				  const char *name, char *buf, size_t size)
{
	int err = 0;
	size_t len;
	struct inode *inode = CAS_TO_INODE(node);

	cas_lock_node(inode);

	if (node->backend_type != cas_type_ari) {
		err = -EOPNOTSUPP;
		goto out;
	}
	if (!size)
		buf = NULL;

	len = snprintf(buf, size, "%s:%lld", node->ari, i_size_read(inode));

out:
	cas_unlock_node(inode);

	if (err)
		return err;

	if (!size)
		return len + 1;

	if (unlikely(size <= len))
		return -ERANGE;

	return len;
}
static int cas_xattr_set_hash_size(struct dentry *dentry, struct cas_node *node,
				   const char *name, const char *buf,
				   size_t len)
{
	int err;
	char *sep;
	struct cas_hash hash;
	uint64_t size;
	substring_t size_str;

	switch (node->type) {
	case CAS_TYPE_REG:
	case CAS_TYPE_REG_EXEC:
		sep = strnchr(buf, len, ':');
		if (unlikely(!sep))
			return -EINVAL;
		err = cas_hash_from_hex(buf, sep - buf, &hash);
		if (unlikely(err))
			return err;
		size_str.from = sep + 1;
		size_str.to = (char *)(buf + len);
		if (match_u64(&size_str, &size))
			return -EINVAL;

		err = cas_force_file_hash(dentry, &hash, size);
		break;
	case CAS_TYPE_DIR:
		/* setting hash for directories with size is not supported */
	default:
		err = -EOPNOTSUPP;
	}
	return err;
}
static int cas_xattr_set_ari_size(struct dentry *dentry, struct cas_node *node,
				  const char *name, const char *buf, size_t len)
{
	int err;
	const char *sep = NULL;
	uint64_t size;
	substring_t size_str;
	size_t i;

	switch (node->type) {
	case CAS_TYPE_REG:
	case CAS_TYPE_REG_EXEC:
		for (i = len; i > 0; i--) {
			if (buf[i - 1] == ':') {
				sep = &buf[i - 1];
				break;
			}
		}
		if (unlikely(!sep || sep == buf || sep >= buf + len - 1))
			return -EINVAL;

		size_str.from = (char *)(sep + 1);
		size_str.to = (char *)(buf + len);
		if (match_u64(&size_str, &size))
			return -EINVAL;

		err = cas_force_file_ari(dentry, buf, sep - buf, size);
		break;
	default:
		err = -EOPNOTSUPP;
	}
	return err;
}
static int cas_xattr_get_hash_stat(struct dentry *dentry, struct cas_node *node,
				   const char *name, char *buf, size_t size)
{
	int err = 0;
	struct cas_hash hash;
	uint64_t hash_size;
	size_t len;
	int mode = 0;

	if (cas_type_is_reg_file(node->type)) {
		err = cas_file_snapshot(node);
		if (unlikely(err))
			return err;
	}

	cas_get_node_hash(node, &hash, &hash_size);

	if (cas_type_is_dir(node->type))
		hash_size = 0;

	mode = node->inode.i_mode;
	if (unlikely(err))
		return err;

	if (!size)
		buf = NULL;

	len = snprintf(buf, size, "%20phN:%lld:%d", &hash, hash_size, mode);

	if (!size)
		return len + 1;

	if (unlikely(size <= len))
		return -ERANGE;

	return len;
}

static int cas_xattr_get_dir_list_hash(struct dentry *dentry,
				       struct cas_node *node, const char *name,
				       char *buf, size_t size)
{
	int err;
	int len;
	struct cas_hash hash;

	switch (node->type) {
	case CAS_TYPE_COMMIT:
	case CAS_TYPE_DIR:
		err = cas_get_dir_list_hash(node, &hash);
		if (unlikely(err))
			return err;

		if (!size)
			buf = NULL;
		len = snprintf(buf, size, "%20phN", &hash);
		if (!size)
			return len + 1;
		if (unlikely(size <= len))
			return -ERANGE;
		return len;
	default:
		err = -EOPNOTSUPP;
	}
	return err;
}

static int cas_xattr_set_deeptree(struct dentry *dentry, struct cas_node *node,
				  const char *name, const char *buf, size_t len)
{
	int err;
	struct cas_hash hash;

	switch (node->type) {
	case CAS_TYPE_DIR:
		err = cas_hash_from_hex(buf, (int)len, &hash);
		if (unlikely(err))
			return err;
		err = cas_dir_set_deeptree(dentry, &hash);
		cas_make_dirty(dentry);
		break;
	default:
		err = -EOPNOTSUPP;
	}
	return err;
}

static int cas_xattr_get_loaded(struct dentry *dentry, struct cas_node *node,
				const char *name, char *buf, size_t size)
{
	int n;

	if (!size)
		return 1;

	if (unlikely(size < 1))
		return -ERANGE;

	n = snprintf(buf, size, "%d", node->is_loaded);

	return n;
}

static int cas_xattr_set_loaded(struct dentry *dentry, struct cas_node *node,
				const char *name, const char *buf, size_t size)
{
	if (size != 1 || (buf[0] != '0' && buf[0] != '1'))
		return -EINVAL;

	/* TODO implement load and unload as necessary */

	return -EOPNOTSUPP;
}

static int cas_xattr_get_debug(struct dentry *dentry, struct cas_node *node,
			       const char *name, char *buf, size_t size)
{
	int n;

	if (!size)
		return 1;

	if (unlikely(size < 1))
		return -ERANGE;

	n = snprintf(buf, size, "%d", node->debug);

	return n;
}

static int cas_xattr_set_debug(struct dentry *dentry, struct cas_node *node,
			       const char *name, const char *buf, size_t size)
{
	if (size != 1 || (buf[0] != '0' && buf[0] != '1'))
		return -EINVAL;

	node->debug = (buf[0] == '1');

	return 0;
}

static int cas_xattr_get_dirty(struct dentry *dentry, struct cas_node *node,
			       const char *name, char *buf, size_t size)
{
	int n;
	bool dirty = false;

	if (!size)
		return 1;

	if (unlikely(size < 1))
		return -ERANGE;

	if (node->type == CAS_TYPE_CTRL)
		return -EOPNOTSUPP;

	dirty = cas_node_is_dirty(node);

	n = snprintf(buf, size, "%d", dirty ? 1 : 0);
	return n;
}

static int cas_xattr_get_node_type(struct dentry *dentry, struct cas_node *node,
				   const char *name, char *buf, size_t size)
{
	int n;
	const char *type = "???";

	switch (node->type) {
	case CAS_TYPE_REG:
		type = "reg";
		break;
	case CAS_TYPE_REG_EXEC:
		type = "reg_exec";
		break;
	case CAS_TYPE_DIR:
		type = "dir";
		break;
	case CAS_TYPE_COMMIT:
		type = "commit";
		break;
	case CAS_TYPE_SYMLINK:
		type = "symlink";
		break;
	case CAS_TYPE_FIFO:
		type = "fifo";
		break;
	case CAS_TYPE_SOCKET:
		type = "socket";
		break;
	case CAS_TYPE_CTRL:
		type = "ctrl";
		break;
	default:
		break;
	}

	n = snprintf(buf, size, "%s", type);

	if (!size)
		return n + 1;

	if (size <= n)
		return -ERANGE;

	return n;
}

static int cas_xattr_get_exclude(struct dentry *dentry, struct cas_node *node,
				 const char *name, char *buf, size_t size)
{
	int n;

	if (!size)
		return 1;

	if (unlikely(size < 1))
		return -ERANGE;

	n = snprintf(buf, size, "%d", node->is_excluded);

	return n;
}

static int cas_xattr_set_exclude(struct dentry *dentry, struct cas_node *node,
				 const char *name, const char *buf, size_t size)
{
	bool excluded;

	if (size != 1 || (buf[0] != '0' && buf[0] != '1'))
		return -EINVAL;

	excluded = (buf[0] == '1');

	if (unlikely(node->is_excluded == excluded))
		return 0;

	node->is_excluded = excluded;

	if (!IS_ROOT(dentry))
		cas_dir_changed(dentry->d_parent);

	return 0;
}

static int cas_xattr_get_status(struct cas_node *node, char *buf, size_t size)
{
	size_t len = 0;
	struct cas_fs *fs = SB_TO_FS(CAS_TO_INODE(node)->i_sb);

	if (!size)
		buf = NULL;

	len = snprintf(buf, size, "%d", fs->ready);

	if (!size)
		return len + 1;

	if (unlikely(size <= len))
		return -ERANGE;

	return len;
}

int cas_xattr_get(const struct xattr_handler *handler, struct dentry *dentry,
		  struct inode *inode, const char *name, void *buffer,
		  size_t size)
{
	int err = 0;
	struct cas_node *node = INODE_TO_CAS(inode);

	err = cas_fs_check(SB_TO_FS(inode->i_sb));
	if (unlikely(err))
		return err;

	pr_debug("%s %llu %20phN name=%s size=%ld\n", __func__,
		 (unsigned long long)inode->i_ino, &node->hash, name, size);

	if (!strcmp(name, CAS_XATTR_STATUS))
		return cas_xattr_get_status(node, buffer, size);

	if (!strcmp(name, CAS_XATTR_TYPE))
		return cas_xattr_get_node_type(dentry, node, name, buffer,
					       size);

	if (node->type == CAS_TYPE_CTRL)
		return -EOPNOTSUPP;

	if (!strcmp(name, CAS_XATTR_HASH))
		return cas_xattr_get_hash(dentry, node, name, buffer, size);

	if (!strcmp(name, CAS_XATTR_BIN_HASH))
		return cas_xattr_get_bin_hash(dentry, node, name, buffer, size);

	if (!strcmp(name, CAS_XATTR_BACKEND_STAT))
		return cas_xattr_get_backend_stat(dentry, node, name, buffer,
						  size);

	if (!strcmp(name, CAS_XATTR_HASH_SIZE))
		return cas_xattr_get_hash_size(dentry, node, name, buffer,
					       size);
	if (!strcmp(name, CAS_XATTR_ARI_SIZE))
		return cas_xattr_get_ari_size(dentry, node, name, buffer, size);

	if (!strcmp(name, CAS_XATTR_HASH_STAT))
		return cas_xattr_get_hash_stat(dentry, node, name, buffer,
					       size);

	if (!strcmp(name, CAS_XATTR_DIR_LIST_HASH))
		return cas_xattr_get_dir_list_hash(dentry, node, name, buffer,
						   size);

	if (!strcmp(name, CAS_XATTR_LOADED))
		return cas_xattr_get_loaded(dentry, node, name, buffer, size);

	if (!strcmp(name, CAS_XATTR_DEBUG))
		return cas_xattr_get_debug(dentry, node, name, buffer, size);

	if (!strcmp(name, CAS_XATTR_DIRTY))
		return cas_xattr_get_dirty(dentry, node, name, buffer, size);

	if (!strcmp(name, CAS_XATTR_EXCLUDE))
		return cas_xattr_get_exclude(dentry, node, name, buffer, size);

	return -EOPNOTSUPP;
}

int cas_xattr_set(const struct xattr_handler *handler,
		  struct mnt_idmap_type *mnt_userns, struct dentry *dentry,
		  struct inode *inode, const char *name, const void *buffer,
		  size_t size, int flags)
{
	int err = 0;
	struct cas_node *node = INODE_TO_CAS(inode);

	err = cas_fs_check(SB_TO_FS(inode->i_sb));
	if (unlikely(err))
		return err;

	if (!strcmp(name, CAS_XATTR_CTRL))
		return cas_ctrl_xattr_set(dentry, inode, name, buffer, size);

	if (node->type == CAS_TYPE_CTRL)
		return -EOPNOTSUPP;

	pr_debug("%s %llu %20phN name=%s value='%.*s' flags=%d\n", __func__,
		 (unsigned long long)inode->i_ino, &node->hash, name, (int)size,
		 (char *)buffer, flags);

	if (!strcmp(name, CAS_XATTR_HASH))
		return cas_xattr_set_hash(dentry, node, name, buffer, size);

	if (!strcmp(name, CAS_XATTR_HASH_SIZE))
		return cas_xattr_set_hash_size(dentry, node, name, buffer,
					       size);
	if (!strcmp(name, CAS_XATTR_ARI_SIZE))
		return cas_xattr_set_ari_size(dentry, node, name, buffer, size);

	if (!strcmp(name, CAS_XATTR_DEEPTREE))
		return cas_xattr_set_deeptree(dentry, node, name, buffer, size);

	if (!strcmp(name, CAS_XATTR_LOADED))
		return cas_xattr_set_loaded(dentry, node, name, buffer, size);

	if (!strcmp(name, CAS_XATTR_DEBUG))
		return cas_xattr_set_debug(dentry, node, name, buffer, size);

	if (!strcmp(name, CAS_XATTR_EXCLUDE))
		return cas_xattr_set_exclude(dentry, node, name, buffer, size);

	return -EOPNOTSUPP;
}
