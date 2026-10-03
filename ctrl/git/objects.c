// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 */

#include <linux/ctype.h>
#include <linux/fs_stack.h>
#include <linux/vmalloc.h>

#include "../../casfs.h"
#include "inode.h"
#include "ctrl.h"
#include "../util.h"
#include "../../signal.h"
#include "../../fstream.h"
#include "../../compat.h"
#include "../../compat_file.h"
#include "../../compat_dir.h"
#include "compat_dir.h"

static int git_to_cas_object_copy(struct cas_provider *provider,
				  struct file *git_file,
				  enum cas_req_obj_type obj_type,
				  const struct cas_hash *hash);

static int cas_hash_from_dentry(struct dentry *dentry, struct cas_hash *hash)
{
	char name[CASFS_HASH_HEX_LEN + 1];

	snprintf(name, CASFS_HASH_HEX_LEN + 1, "%s%s",
		 dentry->d_parent->d_name.name, dentry->d_name.name);

	return cas_hash_from_hex(name, strlen(name), hash);
}

static bool is_valid_hex(const char *name, int len)
{
	int i = 0;

	for (; i < len; i++) {
		if (!isxdigit(name[i]))
			return false;
	}
	return true;
}

static bool is_valid_object_dir_name(struct dentry *dentry)
{
	int len = strlen(dentry->d_name.name);

	if ((len == 2) || (len == 3))
		return is_valid_hex(dentry->d_name.name, len);

	return false;
}

static bool is_valid_object_file_name(struct dentry *dentry)
{
	int plen = strlen(dentry->d_parent->d_name.name);
	int len = strlen(dentry->d_name.name);

	if ((plen + len) == CASFS_HASH_HEX_LEN)
		return is_valid_hex(dentry->d_name.name, len);

	return false;
}

static struct dentry *gitfs_objects_dir_lookup(struct inode *dir,
					       struct dentry *dentry,
					       unsigned int flags)
{
	struct dentry *ret;
	struct dentry *target;

	ret = cas_lookup(dir, dentry, flags);
	if (IS_ERR(ret))
		return ret;

	target = ret ? ret : dentry;

	if (d_really_is_negative(target) && is_valid_object_dir_name(target)) {
		struct dentry *mkdir_ret;

		mkdir_ret = cas_mkdir_internal(dir, target,
					       CAS_TYPE_GIT_OBJECT_DIR, 0755);

		if (IS_ERR(mkdir_ret)) {
			if (ret)
				dput(ret);
			return mkdir_ret;
		}

		if (mkdir_ret) {
			if (ret)
				dput(ret);
			return mkdir_ret;
		}
	}

	return ret;
}
struct dentry *__gitfs_objects_dir_mkdir(struct mnt_idmap_type *mnt_userns,
					 struct inode *dir,
					 struct dentry *dentry, umode_t mode)
{
	if (is_valid_object_dir_name(dentry)) {
		return cas_mkdir_internal(dir, dentry, CAS_TYPE_GIT_OBJECT_DIR,
					  mode);
	}
	return cas_mkdir(mnt_userns, dir, dentry, mode);
}

static int git_commit_new(struct inode *old_dir, struct dentry *old_dentry,
			  struct dentry *new_dentry)
{
	int rc = 0;
	struct cas_hash hash;
	char name[CASFS_HASH_HEX_LEN + 1];
	struct file *lower_file = NULL;
	struct cas_node *node;
	struct cas_fs *fs = SB_TO_FS(old_dir->i_sb);
	struct git_ctrl *git_ctrl = SB_TO_CAS_CTRL(old_dir->i_sb)->git;
	enum cas_req_obj_type obj_type;
	unsigned int nofs_flags;

	snprintf(name, CASFS_HASH_HEX_LEN + 1, "%s%s",
		 new_dentry->d_parent->d_name.name, new_dentry->d_name.name);

	rc = cas_hash_from_hex(name, CASFS_HASH_HEX_LEN, &hash);
	if (rc != 0)
		return rc;

	/* Get the internal node details directly */
	node = INODE_TO_CAS(d_inode(old_dentry));
	if (!node->blob.path.dentry)
		return -ENOENT;

	lower_file = dentry_open(&node->blob.path, O_RDONLY | O_LARGEFILE,
				 current_cred());
	if (IS_ERR(lower_file))
		return PTR_ERR(lower_file);

	/*
	 * Scoped safety block: ensures any allocations within cryptolibs,
	 * zlib, or local buffers are automatically marked GFP_NOFS.
	 */
	nofs_flags = memalloc_nofs_save();

	/* peek_git_type will now use a clean, non-recursive real storage file descriptor */
	rc = peek_git_type(git_ctrl, lower_file, &obj_type);
	if (rc != 0)
		goto out_restore;

	if (obj_type != cas_type_blob) {
		rc = git_to_cas_object_copy(&fs->provider, lower_file, obj_type,
					    &hash);
	} else {
		rc = git_to_cas_object_write(git_ctrl, &fs->provider,
					     lower_file, &hash);
	}

out_restore:
	memalloc_nofs_restore(nofs_flags);
	filp_close(lower_file, NULL);
	return rc;
}

static int
gitfs_objects_dir_rename(struct mnt_idmap_type *idmap, struct inode *old_dir,
			 struct dentry *old_dentry, struct inode *new_dir,
			 struct dentry *new_dentry, unsigned int flags)
{
	int rc = 0;

	if (is_valid_object_file_name(new_dentry)) {
		rc = git_commit_new(old_dir, old_dentry, new_dentry);
		if (rc != 0)
			return rc;
	}

	return cas_rename(idmap, old_dir, old_dentry, new_dir, new_dentry,
			  flags);
}
static int gitfs_objects_dir_link(struct dentry *old_dentry, struct inode *dir,
				  struct dentry *new_dentry)
{
	int rc = 0;

	if (is_valid_object_file_name(new_dentry)) {
		rc = git_commit_new(dir, old_dentry, new_dentry);
		if (rc != 0)
			return rc;
	}

	return cas_link(old_dentry, dir, new_dentry);
}
const struct inode_operations gitfs_objects_dir_inode_operations = {
	/// .git/objects overrides of lookup/mkdir/rename
	.lookup = gitfs_objects_dir_lookup,
	.mkdir = gitfs_objects_dir_mkdir,

	.rename = gitfs_objects_dir_rename,
	.link = gitfs_objects_dir_link,

	.symlink = cas_symlink,
	.unlink = cas_unlink,
	.rmdir = cas_rmdir,

	.create = cas_create,
	.mknod = cas_mknod,
	.getattr = cas_getattr,

};

int create_git_object_dir_locked(struct dentry *parent, const char *dir_name,
				 size_t dir_name_len)
{
	int rc = 0;
	struct dentry *dentry = NULL;
	struct dentry *mkdir_dentry = NULL;

	dentry = casfs_lookup_one(casfs_nop_idmap, dir_name, parent,
				  dir_name_len);
	if (IS_ERR_OR_NULL(dentry)) {
		rc = dentry ? PTR_ERR(dentry) : -ENOMEM;
		goto out_err;
	}
	if (d_really_is_positive(dentry)) {
		rc = -EEXIST;
		goto out_put;
	}
	mkdir_dentry = cas_mkdir_internal(d_inode(parent), dentry,
					  CAS_TYPE_GIT_OBJECTS_DIR, 0);

	if (IS_ERR(mkdir_dentry))
		rc = PTR_ERR(mkdir_dentry);

out_put:
	dput(dentry);
out_err:
	return rc;
}
/* Manually copy bytes from casfs to cas store (for non-blobs) */
static int git_to_cas_object_copy(struct cas_provider *provider,
				  struct file *git_file,
				  enum cas_req_obj_type obj_type,
				  const struct cas_hash *hash)
{
	int rc = 0, rrc = 0;
	loff_t rd_pos = 0, wr_pos = 0;
	ssize_t n, rd, written = 0;
	char *buf;
	struct cas_provider_file *file;
	struct kvec kio; /* Use kvec instead of iovec for kernel addresses */
	struct iov_iter iter;

	file = cas_provider_new_file(provider, obj_type);
	if (IS_ERR(file))
		return PTR_ERR(file);

	buf = vmalloc(PAGE_SIZE);
	if (unlikely(!buf)) {
		rc = -ENOMEM;
		goto out_abort;
	}

	while (rc == 0) {
		kio.iov_base = buf;
		kio.iov_len = PAGE_SIZE;

		iov_iter_kvec(&iter, ITER_DEST, &kio, 1, PAGE_SIZE);

		rd = vfs_iter_read(git_file, &iter, &rd_pos, 0);
		if (rd <= 0) {
			if (rd < 0)
				rc = rd;
			break;
		}

		written = 0;
		for (;;) {
			n = cas_provider_file_write(file, buf + written,
						    rd - written, &wr_pos);
			if (n <= 0) {
				if (n < 0)
					rc = n;
				break;
			}
			written += n;
		}
	}

	vfree(buf);
	if (rc == 0)
		return cas_provider_file_close(file, hash);

out_abort:
	rrc = cas_provider_file_abort(file);
	if (rrc != 0)
		pr_err("failed to abort file %d", rrc);

	return rc;
}

static int gitfs_object_dir_lookup_blocked(struct inode *dir,
					   struct dentry *dentry,
					   unsigned int flags)
{
	int rc = 0;
	struct cas_fs *fs = SB_TO_FS(dir->i_sb);
	struct cas_hash hash;
	struct cas_stat_info stat;
	struct inode *new_inode;

	rc = cas_hash_from_dentry(dentry, &hash);
	if (rc != 0)
		return rc;

	/* Check if known object/blob via provider */
	rc = cas_provider_query_stat(&fs->provider, &hash, &stat);
	if (rc != 0)
		return rc;

	switch (stat.type) {
	case cas_type_blob:
		stat.size = get_compressed_blob_size(stat.size);
		break;
	case cas_type_tree:
	case cas_type_object:
		break;
	default:
		return -ENOENT;
	}

	// Create our CAS regular file
	new_inode = cas_new_node(dir->i_sb, CAS_TYPE_REG, &hash, stat.size,
				 S_IFREG | 0444);

	if (IS_ERR_OR_NULL(new_inode))
		return new_inode ? PTR_ERR(new_inode) : -ENOMEM;

	INODE_TO_CAS(new_inode)->backend_type = stat.type;
	new_inode->i_fop = &gitfs_object_file_operations;
	cas_lock_node(dir);
	rc = cas_dir_add_child(dir, dentry, new_inode);
	cas_unlock_node(dir);

	if (unlikely(rc))
		iput(new_inode);

	return rc;
}

static struct dentry *gitfs_object_dir_lookup(struct inode *dir,
					      struct dentry *dentry,
					      unsigned int flags)
{
	struct dentry *ret;
	struct dentry *target;

	ret = cas_lookup(dir, dentry, flags);
	if (IS_ERR(ret))
		return ret;

	target = ret ? ret : dentry;
	if (d_really_is_negative(target) && is_valid_object_file_name(dentry)) {
		sigset_t oldset;
		int err;
		unsigned int nofs_flags;

		block_signals(&oldset);

		/* Guard dcache inode allocation and lookup from reclaim recursion */
		nofs_flags = memalloc_nofs_save();
		err = gitfs_object_dir_lookup_blocked(dir, target, flags);
		memalloc_nofs_restore(nofs_flags);

		unblock_signals(&oldset);

		if (err) {
			if (ret)
				dput(ret);

			/*
			 * Don't d_drop() the negative -- it leaks the libfs pin at
			 * umount; casfs_revalidate() handles retry.
			 */
			if (err == -ENOENT)
				return NULL;

			return ERR_PTR(err);
		}
	}

	return ret;
}

static int gitfs_object_file_open(struct inode *inode, struct file *file)
{
	int rc;
	struct cas_fs *fs = SB_TO_FS(inode->i_sb);
	struct cas_node *node = INODE_TO_CAS(inode);
	struct path backend_path;
	struct file *scratch_file, *backend_file;
	struct git_ctrl *git_ctrl = SB_TO_CAS_CTRL(inode->i_sb)->git;
	loff_t size;

	if (node->backend_type != cas_type_blob)
		return cas_open(inode, file);

	cas_lock_node(inode);

	if (node->blob.cache_state != CAS_CACHE_UNLOADED) {
		cas_unlock_node(inode);
		return cas_open(inode, file);
	}

	scratch_file = cas_store_create_scratch_file_inum(&fs->cache.blobs,
							  inode->i_ino);
	if (IS_ERR(scratch_file)) {
		rc = PTR_ERR(scratch_file);
		cas_unlock_node(inode);
		return rc;
	}

	// We are only dealing with blobs so we use cas_provider_get_blob_path which
	// will download the blob from the provider as needed
	rc = cas_provider_get_blob_path(&fs->provider, &node->hash,
					&backend_path);
	if (rc != 0) {
		cas_unlock_node(inode);
		filp_close(scratch_file, NULL);
		return rc;
	}

	cas_unlock_node(inode);

	backend_file = dentry_open(&backend_path, O_RDONLY | O_LARGEFILE,
				   current_cred());
	if (IS_ERR(backend_file)) {
		rc = PTR_ERR(backend_file);
		goto out_put_path;
	}

	/* format the data into the scratch file */
	size = cas_to_git_object_write(git_ctrl, true, backend_file,
				       scratch_file);
	if (size >= 0) {
		vfs_fsync(scratch_file, 0);
		cas_file_scratch_setup_external(node, &scratch_file->f_path,
						size);

		rc = 0;
	} else {
		rc = -EINVAL;
	}

	filp_close(backend_file, NULL);
out_put_path:
	path_put(&backend_path);
	filp_close(scratch_file, NULL);

	if (rc == 0)
		rc = cas_open(inode, file);

	return rc;
}

const struct inode_operations gitfs_object_dir_inode_operations = {
	/// .git/objects/xx directory override lookup
	.lookup = gitfs_object_dir_lookup,
	.rename = gitfs_objects_dir_rename,
	.link = gitfs_objects_dir_link,

	.mkdir = cas_mkdir_op,
	.symlink = cas_symlink,
	.unlink = cas_unlink,
	.rmdir = cas_rmdir,
	.create = cas_create,
	.mknod = cas_mknod,
	.getattr = cas_getattr,
};

const struct file_operations gitfs_object_file_operations = {
	// .git/objects/xx -- file override open
	.open = gitfs_object_file_open,

	.release = cas_release,
	.llseek = cas_llseek,
	.read_iter = cas_read_iter,
	.write_iter = cas_write_iter,
	.fsync = cas_fsync,
	.mmap = cas_mmap,
	.fallocate = cas_fallocate,
	.fadvise = cas_fadvise,
	.flush = cas_flush,
	.splice_read = cas_splice_read,
	.splice_write = cas_splice_write,

};
