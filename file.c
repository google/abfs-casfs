// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 */

#include <uapi/linux/stat.h>
#include <linux/pagemap.h>
#include <linux/cred.h>
#include <linux/file.h>
#include <linux/mount.h>
#include <linux/slab.h>
#include <linux/xattr.h>

#include "casfs.h"
#include "node.h"
#include "blob.h"
#include "compat_file.h"
#include "compat_inode.h"
#include "cas_tap.h"
#include "signal.h"

struct cas_file_private {
	struct file *real_file;
	struct path user_path;
	int real_file_generation;
};

static int cas_set_file_size(struct dentry *dentry, struct cas_node *node,
			     loff_t size);

static int cas_setattr_reg_file(struct dentry *dentry, struct iattr *attr,
				bool *touched, bool *update_ctime)
{
	int err;
	unsigned int ia_valid = attr->ia_valid;
	struct inode *inode = d_inode(dentry);
	struct cas_node *node = INODE_TO_CAS(inode);
	bool req_exec = (attr->ia_mode & 0100) != 0;

	if (ia_valid & ATTR_SIZE) {
		err = cas_set_file_size(dentry, node, attr->ia_size);
		if (unlikely(err))
			return err;
		*touched = true;
		casfs_inode_set_mtime_to_ts(inode, current_time(inode));
	}

	if (ia_valid & ATTR_MODE) {
		cas_lock_node(inode);
		node->type = req_exec ? CAS_TYPE_REG_EXEC : CAS_TYPE_REG;
		inode->i_mode = attr->ia_mode;
		cas_unlock_node(inode);
		*touched = true;
		*update_ctime = true;
	}

	return 0;
}

int cas_setattr(struct mnt_idmap_type *mnt_userns, struct dentry *dentry,
		struct iattr *attr)
{
	int err;
	struct inode *inode = d_inode(dentry);
	struct cas_node *node = INODE_TO_CAS(inode);
	bool touched = false, update_ctime = false, gen_tap = false;
	unsigned int ia_valid = attr->ia_valid;
	struct cas_fs *fs = SB_TO_FS(inode->i_sb);

	err = cas_fs_check(fs);
	if (unlikely(err))
		return err;

	pr_debug("%s %llu valid=%d mode=%o size=%lld\n", __func__,
		 (unsigned long long)inode->i_ino, ia_valid, attr->ia_mode,
		 attr->ia_size);

	err = setattr_prepare(mnt_userns, dentry, attr);
	if (unlikely(err))
		return err;

	setattr_copy(mnt_userns, inode, attr);
	mark_inode_dirty(inode);

	if (cas_type_is_reg_file(node->type)) {
		err = cas_setattr_reg_file(dentry, attr, &touched,
					   &update_ctime);
		if (unlikely(err))
			return err;

		gen_tap = touched;
	}

	if (ia_valid & (ATTR_ATIME | ATTR_MTIME | ATTR_CTIME)) {
		if (ia_valid & ATTR_ATIME)
			casfs_inode_set_atime_to_ts(inode, attr->ia_atime);
		if (ia_valid & ATTR_MTIME) {
			casfs_inode_set_mtime_to_ts(inode, attr->ia_mtime);
			gen_tap = true;
		}
		if (ia_valid & ATTR_CTIME) {
			casfs_inode_set_ctime_to_ts(inode, attr->ia_ctime);
			gen_tap = true;
		}
		touched = true;
	}

	if (update_ctime && (ia_valid & ATTR_CTIME) == 0) {
		casfs_inode_set_ctime_current(inode);
		gen_tap = true;
	}

	if (gen_tap) {
		cas_lock_node(inode);
		cas_tap_attr_change(dentry);
		cas_unlock_node(inode);
	}
	if (touched && !node->is_excluded)
		cas_dir_changed(dentry->d_parent);

	return 0;
}

const struct inode_operations cas_file_inode_operations = {
	.setattr = cas_setattr,
	.getattr = cas_getattr,
};

static void cas_file_set_hash(struct cas_node *node,
			      const struct cas_hash *hash, loff_t size)
{
	kfree(node->ari);
	node->ari = NULL;
	WRITE_ONCE(node->backend_type, cas_type_blob);
	cas_set_node_hash(node, hash, size);
}
int cas_force_file_hash(struct dentry *dentry, const struct cas_hash *hash,
			uint64_t size)
{
	int err = 0;
	struct inode *inode = d_inode(dentry);
	struct cas_node *node = INODE_TO_CAS(inode);

	cas_lock_node(inode);

	if (cas_node_is_dirty(node)) {
		pr_warn_ratelimited(
			"%s cannot set hash %20phN for dirty file %pd\n",
			__func__, hash, dentry);
		err = -EINVAL;
		goto out;
	}
	if (!cas_hash_cmp(&node->hash, hash)) {
		err = 0;
		goto out;
	}

	cas_blob_reset(&node->blob);
	cas_file_set_hash(node, hash, size);

	node->is_loaded = false;
	cas_touch_mtime_ctime(inode);
out:
	cas_unlock_node(inode);

	if (likely(!err && !node->is_excluded))
		cas_dir_changed(dentry->d_parent);

	return err;
}
int cas_force_file_ari(struct dentry *dentry, const char *ari, int ari_len,
		       uint64_t size)
{
	int err = 0;
	struct inode *inode = d_inode(dentry);
	struct cas_node *node = INODE_TO_CAS(inode);
	char *new_ari;

	if (ari == NULL || ari_len <= 0 || ari_len > CAS_MAX_ARI_LEN)
		return -EINVAL;

	cas_lock_node(inode);

	if (cas_node_is_dirty(node)) {
		pr_warn_ratelimited(
			"%s cannot set ari %.*s for dirty file %pd\n", __func__,
			ari_len, ari, dentry);
		err = -EINVAL;
		goto out;
	}
	if (node->backend_type == cas_type_ari && node->ari &&
	    strnlen(node->ari, ari_len + 1) == ari_len &&
	    memcmp(node->ari, ari, ari_len) == 0) {
		goto out;
	}

	// Store the data
	new_ari = kstrndup(ari, ari_len, GFP_KERNEL);
	if (!new_ari) {
		err = -ENOMEM;
		goto out;
	}
	kfree(node->ari);
	node->ari = new_ari;
	cas_blob_reset(&node->blob);
	i_size_write(CAS_TO_INODE(node), size);
	WRITE_ONCE(node->backend_type, cas_type_ari);
	node->is_loaded = false;
	cas_touch_mtime_ctime(inode);
out:
	cas_unlock_node(inode);

	if (likely(!err && !node->is_excluded))
		cas_dir_changed(dentry->d_parent);

	return err;
}

struct file *cas_ensure_real_file_locked(struct file *file)
{
	struct cas_file_private *priv = file->private_data;
	struct inode *inode = file_inode(file);
	struct cas_node *node = INODE_TO_CAS(inode);
	struct file *real_file, *old_file = NULL;
	int flags;
	int current_gen;

	current_gen = atomic_read(&node->blob.generation);
	real_file = priv->real_file;

	if (real_file) {
		if (priv->real_file_generation == current_gen)
			return real_file;

		/* Stale generation detected; stage old descriptor for closure */
		old_file = real_file;
		priv->real_file = NULL;
	}
	if (!node->blob.path.dentry ||
	    d_really_is_negative(node->blob.path.dentry)) {
		real_file = ERR_PTR(-ENOENT);
		goto out_close;
	}

	if (file->f_mode & FMODE_WRITE)
		flags = O_RDWR | O_LARGEFILE;
	else
		flags = O_RDONLY | O_LARGEFILE;

	if (file->f_flags & O_APPEND)
		flags |= O_APPEND;
	if (file->f_flags & O_NONBLOCK)
		flags |= O_NONBLOCK;

	/* Open the fresh path matching the current generation */
	real_file = cas_backing_file_open(&priv->user_path, file, flags,
					  &node->blob.path, current_cred());

	if (IS_ERR(real_file)) {
		if (old_file && priv->real_file_generation == current_gen) {
			priv->real_file = old_file;
			real_file = old_file;
			old_file = NULL;
		} else {
			priv->real_file = NULL;
		}
		goto out_close;
	}

	priv->real_file_generation = current_gen;
	priv->real_file = real_file;

out_close:
	if (old_file)
		filp_close(old_file, NULL);

	return real_file;
}

static int cas_file_ensure_read_locked(struct file *file, struct cas_node *node,
				       struct cas_file_private *priv)
{
	int ret;
	struct file *real_file;

	ret = cas_blob_ensure_read(&node->blob);
	if (ret)
		return ret;

	real_file = cas_ensure_real_file_locked(file);
	return IS_ERR(real_file) ? PTR_ERR(real_file) : 0;
}

static int cas_file_ensure_write_locked(struct file *file,
					struct cas_node *node,
					struct cas_file_private *priv)
{
	int ret;
	struct file *real_file;
	struct cas_fs *fs = SB_TO_FS(file_inode(file)->i_sb);

	ret = cas_fs_check(fs);
	if (unlikely(ret))
		return ret;

	ret = cas_blob_ensure_write(&node->blob);
	if (ret)
		return ret;

	real_file = cas_ensure_real_file_locked(file);
	return IS_ERR(real_file) ? PTR_ERR(real_file) : 0;
}

int cas_open(struct inode *inode, struct file *file)
{
	int err;
	struct cas_fs *fs = SB_TO_FS(inode->i_sb);
	struct cas_node *node = INODE_TO_CAS(inode);
	struct cas_file_private *priv;
	bool is_read_only = ((file->f_flags & O_ACCMODE) == O_RDONLY) &&
			    ((file->f_flags & O_CREAT) == 0);

	priv = kmalloc(sizeof(struct cas_file_private), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->real_file = NULL;
	priv->real_file_generation = -1;
	path_get(&file->f_path);
	priv->user_path = file->f_path;

	cas_lock_node(inode);
	if (file->f_flags & O_TRUNC) {
		cas_blob_reset(&node->blob);
		cas_file_set_hash(node, &cas_empty_file_hash, 0);
		i_size_write(inode, 0);
		cas_touch_mtime_ctime(inode);
	}

	err = cas_blob_open(&node->blob);

	if (unlikely(err)) {
		path_put(&priv->user_path);
		kfree(priv);
		cas_unlock_node(inode);
		return err;
	}

	file->private_data = priv;
	if (file->f_flags & O_APPEND)
		file->f_pos = i_size_read(inode);

	cas_tap_open(file, is_read_only);

	if (!cas_node_is_dirty(node))
		cas_provider_queue_notify_use(&fs->provider, &node->hash);

	cas_unlock_node(inode);
	return 0;
}

int cas_release(struct inode *inode, struct file *file)
{
	int notify_parent = 0;
	struct cas_node *node = INODE_TO_CAS(inode);
	struct cas_file_private *priv = file->private_data;

	if (priv) {
		if (priv->real_file)
			filp_close(priv->real_file, NULL);
		path_put(&priv->user_path);
		kfree(priv);
		file->private_data = NULL;
	}

	cas_lock_node(inode);
	if (unlikely(node->is_excluded))
		notify_parent = 0;
	else
		notify_parent = cas_node_is_dirty(node);
	cas_unlock_node(inode);

	if (likely(notify_parent))
		cas_dir_changed(file_dentry(file)->d_parent);

	return 0;
}

loff_t cas_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file_inode(file);
	struct cas_node *node = INODE_TO_CAS(inode);
	struct cas_file_private *priv = file->private_data;
	struct file *real_file;
	loff_t ret;

	if (offset == 0) {
		if (whence == SEEK_CUR)
			return file->f_pos;

		if (whence == SEEK_SET)
			return vfs_setpos(file, 0, 0);
	}

	cas_lock_node(inode);
	ret = cas_file_ensure_read_locked(file, node, priv);
	if (unlikely(ret)) {
		cas_unlock_node(inode);
		return ret;
	}

	real_file = priv->real_file;
	if (IS_ERR_OR_NULL(real_file)) {
		ret = real_file ? PTR_ERR(real_file) : -EIO;
		goto out_unlock;
	}

	real_file->f_pos = file->f_pos;
	ret = vfs_llseek(real_file, offset, whence);
	if (ret >= 0)
		file->f_pos = real_file->f_pos;

out_unlock:
	cas_unlock_node(inode);
	return ret;
}

ssize_t cas_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	struct cas_node *node = INODE_TO_CAS(inode);
	struct cas_file_private *priv = file->private_data;
	struct inode *real_inode;
	ssize_t ret;

	if (!iov_iter_count(iter))
		return 0;

	inode_lock_shared(inode);

	cas_lock_node(inode);
	ret = cas_file_ensure_read_locked(file, node, priv);
	if (unlikely(ret)) {
		cas_unlock_node(inode);
		goto out_unlock;
	}

	if (IS_ERR_OR_NULL(priv->real_file)) {
		ret = priv->real_file ? PTR_ERR(priv->real_file) : -EIO;
		cas_unlock_node(inode);
		goto out_unlock;
	}

	real_inode = file_inode(priv->real_file);

	if (i_size_read(inode) != i_size_read(real_inode))
		i_size_write(inode, i_size_read(real_inode));
	cas_unlock_node(inode);

	file_accessed(file);
	ret = cas_compat_read_iter(file, priv->real_file, iter, iocb);

out_unlock:
	inode_unlock_shared(inode);
	return ret;
}

ssize_t cas_write_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	struct cas_node *node = INODE_TO_CAS(inode);
	struct cas_file_private *priv = file->private_data;
	struct inode *real_inode;
	ssize_t ret;

	if (!iov_iter_count(iter))
		return 0;

	inode_lock(inode);

	cas_lock_node(inode);
	ret = cas_file_ensure_write_locked(file, node, priv);
	if (unlikely(ret)) {
		cas_unlock_node(inode);
		goto out_unlock;
	}

	if (IS_ERR_OR_NULL(priv->real_file)) {
		ret = priv->real_file ? PTR_ERR(priv->real_file) : -EIO;
		cas_unlock_node(inode);
		goto out_unlock;
	}

	cas_unlock_node(inode);

	real_inode = file_inode(priv->real_file);
	file_start_write(priv->real_file);

	ret = cas_compat_write_iter(file, priv->real_file, iter, iocb);

	file_end_write(priv->real_file);

	if (ret > 0) {
		cas_touch_mtime_ctime(inode);
		i_size_write(inode, i_size_read(real_inode));
	}

out_unlock:
	inode_unlock(inode);
	return ret;
}

int cas_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file_inode(file);
	struct cas_node *node = INODE_TO_CAS(inode);
	struct cas_file_private *priv = file->private_data;
	struct file *real_file;
	int ret;

	cas_lock_node(inode);
	ret = cas_file_ensure_read_locked(file, node, priv);
	if (unlikely(ret)) {
		cas_unlock_node(inode);
		return ret;
	}
	real_file = priv->real_file;
	cas_unlock_node(inode);

	if (IS_ERR_OR_NULL(real_file))
		return real_file ? PTR_ERR(real_file) : -EIO;

	return vfs_fsync(real_file, datasync);
}

int cas_mmap(struct file *file, struct vm_area_struct *vma)
{
	int ret;
	struct inode *inode = file_inode(file);
	struct cas_node *node = INODE_TO_CAS(inode);
	struct cas_file_private *priv = file->private_data;
	sigset_t oldset;

	if (WARN_ON(file != vma->vm_file))
		return -EIO;

	block_signals(&oldset);
	cas_lock_node(inode);
	if (vma->vm_flags & VM_SHARED)
		ret = cas_file_ensure_write_locked(file, node, priv);
	else
		ret = cas_file_ensure_read_locked(file, node, priv);
	cas_unlock_node(inode);
	unblock_signals(&oldset);

	if (unlikely(ret))
		return ret;

	return cas_compat_mmap(file, priv->real_file, vma);
}

long cas_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	long ret;
	struct inode *inode = file_inode(file);
	struct cas_node *node = INODE_TO_CAS(inode);
	struct cas_file_private *priv = file->private_data;
	struct inode *real_inode;

	cas_lock_node(inode);
	ret = cas_file_ensure_write_locked(file, node, priv);
	if (unlikely(ret)) {
		cas_unlock_node(inode);
		return ret;
	}

	real_inode = file_inode(priv->real_file);
	ret = vfs_fallocate(priv->real_file, mode, offset, len);

	if (likely(!ret)) {
		i_size_write(CAS_TO_INODE(node), i_size_read(real_inode));
		cas_touch_mtime_ctime(inode);
	}
	cas_unlock_node(inode);

	return ret;
}

int cas_fadvise(struct file *file, loff_t offset, loff_t len, int advice)
{
	struct inode *inode = file_inode(file);
	struct cas_node *node = INODE_TO_CAS(inode);
	struct cas_file_private *priv = file->private_data;
	struct file *real_file;
	int ret;

	cas_lock_node(inode);
	ret = cas_file_ensure_read_locked(file, node, priv);
	if (unlikely(ret)) {
		cas_unlock_node(inode);
		return ret;
	}
	real_file = priv->real_file;
	cas_unlock_node(inode);

	if (IS_ERR_OR_NULL(real_file))
		return real_file ? PTR_ERR(real_file) : -EIO;

	return vfs_fadvise(real_file, offset, len, advice);
}

int cas_flush(struct file *file, fl_owner_t id)
{
	struct inode *inode = file_inode(file);
	struct cas_file_private *priv;
	struct file *real_file;
	int ret = 0;

	cas_lock_node(inode);
	priv = file->private_data;
	if (unlikely(!priv)) {
		cas_unlock_node(inode);
		return 0;
	}

	real_file = priv->real_file;
	if (real_file && real_file->f_op->flush)
		ret = real_file->f_op->flush(real_file, id);
	cas_unlock_node(inode);

	return ret;
}

ssize_t cas_splice_read(struct file *in, loff_t *ppos,
			struct pipe_inode_info *pipe, size_t len,
			unsigned int flags)
{
	struct file *real_file;
	struct inode *inode = file_inode(in);
	struct cas_node *node = INODE_TO_CAS(inode);
	struct cas_file_private *priv = in->private_data;
	ssize_t ret;

	inode_lock_shared(inode);
	cas_lock_node(inode);
	ret = cas_file_ensure_read_locked(in, node, priv);
	if (unlikely(ret)) {
		cas_unlock_node(inode);
		goto out_unlock;
	}
	real_file = priv->real_file;
	cas_unlock_node(inode);

	ret = cas_compat_splice_read(in, real_file, ppos, pipe, len, flags);

	if (ret > 0)
		file_accessed(in);

out_unlock:
	inode_unlock_shared(inode);
	return ret;
}

ssize_t cas_splice_write(struct pipe_inode_info *pipe, struct file *out,
			 loff_t *ppos, size_t len, unsigned int flags)
{
	struct file *real_file;
	struct inode *inode = file_inode(out);
	struct cas_node *node = INODE_TO_CAS(inode);
	struct cas_file_private *priv = out->private_data;
	ssize_t ret;

	inode_lock(inode);
	cas_lock_node(inode);
	ret = cas_file_ensure_write_locked(out, node, priv);
	if (unlikely(ret)) {
		cas_unlock_node(inode);
		goto out_unlock;
	}
	real_file = priv->real_file;
	cas_unlock_node(inode);

	ret = cas_compat_splice_write(pipe, out, real_file, ppos, len, flags);

	if (ret > 0) {
		cas_touch_mtime_ctime(inode);
		i_size_write(inode, i_size_read(file_inode(real_file)));
	}

out_unlock:
	inode_unlock(inode);
	return ret;
}

const struct file_operations cas_file_operations = {
	.open = cas_open,
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

static int cas_set_file_size(struct dentry *dentry, struct cas_node *node,
			     loff_t size)
{
	int err = 0;
	struct inode *inode = CAS_TO_INODE(node);

	cas_lock_node(inode);
	if (size == i_size_read(inode))
		goto out_unlock;

	err = cas_blob_ensure_write(&node->blob);
	if (unlikely(err))
		goto out_unlock;

	err = vfs_truncate(&node->blob.path, size);
	if (likely(err == 0)) {
		i_size_write(CAS_TO_INODE(node), size);
		if (node->inode.i_mapping)
			truncate_setsize(&node->inode, size);
		cas_touch_mtime_ctime(inode);
	}

out_unlock:
	cas_unlock_node(inode);
	return err;
}

void cas_free_file(struct cas_node *node)
{
	struct inode *inode = CAS_TO_INODE(node);

	cas_lock_node(inode);
	kfree(node->ari);
	cas_blob_reset(&node->blob);
	cas_unlock_node(inode);
}

int cas_file_snapshot(struct cas_node *node)
{
	int err;
	struct cas_hash hash;
	loff_t verified_size;
	struct inode *inode = CAS_TO_INODE(node);
	struct cas_fs *fs = SB_TO_FS(inode->i_sb);
	bool updated = false;

	if (READ_ONCE(node->backend_type) == cas_type_ari)
		return -EINVAL;

	if (likely(!cas_node_is_dirty(node)))
		return 0;

	inode_lock(inode);
	cas_lock_node(inode);

	// Double check under lock.
	if (node->backend_type == cas_type_ari) {
		err = -EINVAL;
		goto out_unlock;
	}
	if (node->blob.path.dentry) {
		struct inode *real_inode = d_inode(node->blob.path.dentry);

		if (mapping_writably_mapped(real_inode->i_mapping)) {
			err = -EBUSY;
			goto out_unlock;
		}

		err = filemap_write_and_wait(real_inode->i_mapping);
		if (unlikely(err))
			goto out_unlock;

		verified_size = i_size_read(real_inode);
		if (verified_size != i_size_read(inode))
			i_size_write(inode, verified_size);
	} else {
		verified_size = i_size_read(inode);
	}

	err = cas_blob_snapshot(&node->blob, verified_size, &hash,
				&verified_size, &updated);

	if (unlikely(err) || !updated)
		goto out_unlock;

	cas_file_set_hash(node, &hash, verified_size);
	cas_provider_queue_new_object(&fs->provider, &hash, cas_type_blob);

out_unlock:
	cas_unlock_node(inode);
	inode_unlock(inode);
	return err;
}

void cas_file_scratch_setup_external(struct cas_node *node,
				     const struct path *path, loff_t size)
{
	cas_lock_node(&node->inode);

	cas_blob_scratch_setup_external(&node->blob, path, size);

	i_size_write(CAS_TO_INODE(node), size);
	mark_inode_dirty(&node->inode);
	cas_unlock_node(&node->inode);
}
