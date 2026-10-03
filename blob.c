// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Google LLC
 */
#include <linux/fs.h>
#include <linux/cred.h>
#include <linux/file.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>

#include "casfs.h"
#include "node.h"
#include "provider.h"

static int cas_blob_load(struct cas_blob *blob);

static inline struct cas_hash *cas_blob_hash(struct cas_blob *blob)
{
	struct cas_node *node = container_of(blob, struct cas_node, blob);

	return &node->hash;
}

static inline struct cas_provider *cas_blob_provider(struct cas_blob *blob)
{
	struct cas_node *node = container_of(blob, struct cas_node, blob);
	struct cas_fs *fs = SB_TO_FS(CAS_TO_INODE(node)->i_sb);

	return &fs->provider;
}

static inline enum cas_req_obj_type cas_blob_backend_type(struct cas_blob *blob)
{
	struct cas_node *node = container_of(blob, struct cas_node, blob);

	return (enum cas_req_obj_type)node->backend_type;
}

static inline struct cas_store *cas_blob_store(struct cas_blob *blob)
{
	struct cas_node *node = container_of(blob, struct cas_node, blob);
	struct cas_fs *fs = SB_TO_FS(CAS_TO_INODE(node)->i_sb);

	return &fs->cache.blobs;
}

static inline unsigned long cas_blob_file_inum(struct cas_blob *blob)
{
	struct cas_node *node = container_of(blob, struct cas_node, blob);

	return node->inode.i_ino;
}

static inline bool cas_blob_is_dirty(struct cas_blob *blob)
{
	return cas_node_is_dirty(container_of(blob, struct cas_node, blob));
}

static inline void cas_blob_set_dirty(struct cas_blob *blob)
{
	struct cas_node *node = container_of(blob, struct cas_node, blob);

	atomic_set(&node->dirtiness, 1);
	// Cannot have dirty ARI files
	if (node->backend_type == cas_type_ari) {
		WRITE_ONCE(node->backend_type, cas_type_blob);
		kfree(node->ari);
		node->ari = NULL;
	}
}

static inline void cas_blob_clear_dirty(struct cas_blob *blob)
{
	struct cas_node *node = container_of(blob, struct cas_node, blob);

	atomic_set(&node->dirtiness, 0);
}
static inline void cas_blob_set_file_loaded(struct cas_blob *blob)
{
	struct cas_node *node = container_of(blob, struct cas_node, blob);

	node->is_loaded = true;
}
static inline void cas_blob_path_release(struct cas_blob *blob)
{
	if (blob->path.dentry)
		path_put_init_(&blob->path);
}

static inline void cas_blob_path_set(struct cas_blob *blob,
				     const struct path *path)
{
	if (blob->path.dentry)
		pr_err("Expected empty dentry on blob path set\n");
	blob->path = *path;
}
static inline int cas_blob_provider_path(struct cas_blob *blob,
					 struct path *path)
{
	struct cas_provider *provider = cas_blob_provider(blob);
	struct cas_node *node = container_of(blob, struct cas_node, blob);

	if (node->backend_type == cas_type_ari)
		return cas_provider_ari_path(provider, node->ari, path);

	return cas_provider_path(provider, node->backend_type, &node->hash,
				 path);
}
void cas_blob_scratch_setup_external(struct cas_blob *blob,
				     const struct path *path, loff_t size)
{
	path_get(path);
	cas_blob_path_set(blob, path);
	cas_blob_set_dirty(blob);
	cas_blob_set_file_loaded(blob);

	atomic_inc(&blob->generation);
	blob->cache_state = CAS_CACHE_RESIDENT;
}

void cas_blob_init(struct cas_blob *blob)
{
	blob->cache_state = CAS_CACHE_UNLOADED;
	blob->path = (struct path){};
	cas_blob_clear_dirty(blob);
}

int cas_blob_open(struct cas_blob *blob)
{
	switch (blob->cache_state) {
	case CAS_CACHE_RESIDENT:
	case CAS_CACHE_READ_ACTIVE:
	case CAS_CACHE_WRITE_ACTIVE:
		cas_blob_set_file_loaded(blob);
		return 0;
	case CAS_CACHE_UNLOADED:
		return cas_blob_load(blob);
	default:
		return -1;
	}
}

void cas_blob_sync_cache(struct cas_blob *blob)
{
	bool dirty = cas_blob_is_dirty(blob);

	if (!dirty) {
		cas_blob_path_release(blob);
		blob->cache_state = CAS_CACHE_UNLOADED;
	} else {
		blob->cache_state = CAS_CACHE_RESIDENT;
	}
}

void cas_blob_reset(struct cas_blob *blob)
{
	if (cas_blob_is_dirty(blob) && blob->path.dentry)
		cas_store_remove_scratch(cas_blob_store(blob),
					 blob->path.dentry);

	cas_blob_sync_cache(blob);
	cas_blob_path_release(blob);
	cas_blob_init(blob);

	atomic_inc(&blob->generation);
}

static int cas_copy_file(struct file *src_file, loff_t src_offset,
			 struct file *dest_file, loff_t dest_offset, loff_t len)
{
	void *buf;
	loff_t remaining = len;
	size_t chunk;
	ssize_t bytes_read, bytes_written;
	int ret = 0;

	if (!len)
		return 0;

	buf = (void *)__get_free_page(GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	while (remaining > 0) {
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			break;
		}

		chunk = min_t(loff_t, remaining, PAGE_SIZE);

		bytes_read = kernel_read(src_file, buf, chunk, &src_offset);
		if (bytes_read <= 0) {
			ret = bytes_read ? (int)bytes_read : -EIO;
			break;
		}

		bytes_written =
			kernel_write(dest_file, buf, bytes_read, &dest_offset);
		if (bytes_written < 0) {
			ret = (int)bytes_written;
			break;
		}
		if (bytes_written != bytes_read) {
			ret = -EIO;
			break;
		}
		remaining -= bytes_written;
		cond_resched();
	}

	free_page((unsigned long)buf);
	return ret;
}

static int cas_blob_open_rw(struct cas_blob *blob)
{
	int err = 0;
	loff_t len = 0ull;
	struct file *tmp_file = NULL;
	struct file *old_ro_file = NULL;

	if (blob->cache_state == CAS_CACHE_UNLOADED) {
		err = cas_blob_load(blob);
		if (unlikely(err))
			return err;
	}

	if (blob->cache_state != CAS_CACHE_RESIDENT &&
	    blob->cache_state != CAS_CACHE_READ_ACTIVE &&
	    blob->cache_state != CAS_CACHE_WRITE_ACTIVE) {
		return -EINVAL;
	}

	if (cas_blob_is_dirty(blob)) {
		blob->cache_state = CAS_CACHE_WRITE_ACTIVE;
		return 0;
	}

	tmp_file = cas_store_create_scratch_file_inum(cas_blob_store(blob),
						      cas_blob_file_inum(blob));
	if (IS_ERR(tmp_file))
		return PTR_ERR(tmp_file);

	if (blob->path.dentry) {
		old_ro_file = dentry_open(&blob->path, O_RDONLY | O_LARGEFILE,
					  current_cred());
		if (!IS_ERR(old_ro_file)) {
			len = i_size_read(file_inode(old_ro_file));
			err = cas_copy_file(old_ro_file, 0, tmp_file, 0, len);
			filp_close(old_ro_file, NULL);
			if (unlikely(err))
				goto out_close_tmp;
		}
	}

	cas_blob_path_release(blob);
	path_get(&tmp_file->f_path);
	cas_blob_path_set(blob, &tmp_file->f_path);

	cas_blob_set_dirty(blob);

	atomic_inc(&blob->generation);
	blob->cache_state = CAS_CACHE_WRITE_ACTIVE;

	filp_close(tmp_file, NULL);
	return 0;

out_close_tmp:
	{
		struct path tmp_path = tmp_file->f_path;

		path_get(&tmp_path);
		filp_close(tmp_file, NULL);
		cas_store_remove_scratch(cas_blob_store(blob), tmp_path.dentry);
		path_put(&tmp_path);
	}
	return err;
}

int cas_blob_ensure_read(struct cas_blob *blob)
{
	if ((blob->cache_state == CAS_CACHE_READ_ACTIVE) ||
	    (blob->cache_state == CAS_CACHE_WRITE_ACTIVE))
		return 0;

	if (blob->cache_state == CAS_CACHE_UNLOADED)
		return cas_blob_load(blob);

	blob->cache_state = CAS_CACHE_READ_ACTIVE;
	return 0;
}

int cas_blob_ensure_write(struct cas_blob *blob)
{
	if (blob->cache_state == CAS_CACHE_WRITE_ACTIVE)
		return 0;

	return cas_blob_open_rw(blob);
}

int cas_blob_snapshot(struct cas_blob *blob, const loff_t exp_size,
		      struct cas_hash *out_hash, loff_t *out_size,
		      bool *updated)
{
	struct cas_hash hash;
	loff_t size = 0ull;
	int err = 0;
	struct file *snap_file;

	if (!cas_blob_is_dirty(blob)) {
		*updated = false;
		return 0;
	}

	snap_file = dentry_open(&blob->path, O_RDONLY | O_LARGEFILE,
				current_cred());
	if (IS_ERR(snap_file))
		return PTR_ERR(snap_file);

	err = cas_hash_file(snap_file, &hash, &size, true);
	filp_close(snap_file, NULL);
	if (unlikely(err))
		return err;

	if (size != exp_size)
		pr_err("%s: hash %20phN size %lld expected %lld\n", __func__,
		       &hash, size, exp_size);

	cas_blob_sync_cache(blob);

	err = cas_store_put_file(cas_blob_store(blob), &hash,
				 blob->path.dentry);
	if (unlikely(err)) {
		pr_err("could not put the temp file %pd: %pe\n",
		       blob->path.dentry, ERR_PTR(err));
		return err;
	}

	cas_blob_path_release(blob);
	cas_blob_clear_dirty(blob);
	*out_hash = hash;
	*out_size = size;
	*updated = true;

	atomic_inc(&blob->generation);
	blob->cache_state = CAS_CACHE_UNLOADED;
	return 0;
}

static int cas_blob_load(struct cas_blob *blob)
{
	int err = 0;
	struct path path;

	if (blob->cache_state != CAS_CACHE_UNLOADED)
		return -1;

	err = cas_blob_provider_path(blob, &path);

	if (unlikely(err)) {
		if (cas_blob_backend_type(blob) != cas_type_blob)
			return err;

		// Allow for empty hash to not exist via provider
		if (cas_hash_cmp(cas_blob_hash(blob), &cas_empty_file_hash) !=
		    0)
			return err;
	} else {
		cas_blob_path_set(blob, &path);
	}

	blob->cache_state = CAS_CACHE_RESIDENT;
	cas_blob_set_file_loaded(blob);
	return 0;
}
