// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 */

#include <uapi/linux/stat.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include "cas_zlib.h"
#include "casfs.h"
#include "cache.h"
#include "compat.h"
#include "compat_dir.h"
#include "compat_inode.h"
#include "hash.h"

static char *hex = "0123456789abcdef";

#define CAS_BLOBS_DIR "blobs"
#define CAS_OBJECTS_DIR "objects"
#define CAS_ARIS_DIR "aris"
#define CAS_TREE_META "tree"
#define CAS_SCRATCH_DIR "scratch"

static inline struct mnt_idmap_type *cas_store_ns(struct cas_store *store)
{
	return mnt_idmap_fn(store->top_dir.mnt);
}
static inline struct mnt_idmap_type *cas_scratch_ns(struct cas_scratch *scratch)
{
	return mnt_idmap_fn(scratch->top.mnt);
}

static inline const struct cred *
cas_override_cred_from_inode(struct inode *inode)
{
	int err;
	struct cred *new_cred;

	new_cred = prepare_creds();
	if (unlikely(!new_cred))
		return ERR_PTR(-ENOMEM);

	err = set_create_files_as(new_cred, inode);
	if (unlikely(err)) {
		put_cred(new_cred);
		return ERR_PTR(err);
	}

	return override_creds(new_cred);
}

static void cas_revert_cred(const struct cred *old_cred)
{
	const struct cred *current_cred = current_cred();

	revert_creds(old_cred);
	abort_creds((struct cred *)current_cred);
}

static void cas_log_access_err(const struct path *parent,
			       const char *child_name, int err)
{
	pr_err("cannot access %pd/%s: %pe\n", parent->dentry, child_name,
	       ERR_PTR(err));
}

static int cas_get_dir_path(const char *name, struct path *path)
{
	int err;

	err = kern_path(name, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, path);
	if (unlikely(err)) {
		pr_err("failed to resolve '%s': %pe\n", name, ERR_PTR(err));
		return err;
	}
	return 0;
}

static int cas_get_dir_child(struct path *parent, const char *child_name,
			     unsigned int type, struct path *path)
{
	int err = 0;
	struct dentry *child;

	child = casfs_lookup_one_unlocked(casfs_nop_idmap, child_name,
					  parent->dentry, strlen(child_name));
	if (IS_ERR(child)) {
		err = PTR_ERR(child);
		goto out;
	}
	if (unlikely(d_is_negative(child))) {
		err = -ENOENT;
		goto out_put;
	}
	if (unlikely(child->d_sb != parent->dentry->d_sb)) {
		err = -EXDEV;
		goto out_put;
	}

	switch (type) {
	case DCACHE_DIRECTORY_TYPE:
		if (unlikely(!d_is_dir(child))) {
			err = -ENOTDIR;
			goto out_put;
		}
		break;
	case DCACHE_REGULAR_TYPE:
		if (unlikely(!d_is_reg(child))) {
			err = -EINVAL;
			goto out_put;
		}
		break;
	}

	path->dentry = child;
	path->mnt = mntget(parent->mnt);

	return 0;

out_put:
	dput(child);
out:
	return err;
}
static int cas_mkdir_dir(struct dentry *parent_dentry, const char *new_dir_name)
{
	int err = 0;
	struct dentry *child;
	const struct cred *old_cred;

	child = casfs_lookup_one_unlocked(casfs_nop_idmap, new_dir_name,
					  parent_dentry, strlen(new_dir_name));
	if (IS_ERR(child)) {
		err = PTR_ERR(child);
		goto out;
	}
	if (unlikely(d_really_is_positive(child))) {
		err = -EEXIST;
		goto out_put;
	}

	old_cred = cas_override_cred_from_inode(d_inode(parent_dentry));
	if (IS_ERR(old_cred)) {
		err = PTR_ERR(old_cred);
		goto out_put;
	}

	inode_lock_nested(d_inode(parent_dentry), I_MUTEX_PARENT);

	err = casfs_vfs_mkdir(casfs_nop_idmap, d_inode(parent_dentry), child,
			      0755);

	inode_unlock(d_inode(parent_dentry));
	cas_revert_cred(old_cred);

	/* Post-Creation Fixup Pass: Force 0755 metadata overwrite safely */
	if (err == 0 && child && d_really_is_positive(child)) {
		struct iattr newattrs;

		newattrs.ia_mode = S_IFDIR | 0755;
		newattrs.ia_valid = ATTR_MODE | ATTR_FORCE;

		inode_lock(d_inode(child));
		err = casfs_notify_change(casfs_nop_idmap, child, &newattrs);
		inode_unlock(d_inode(child));
	}

out_put:
	if (child)
		dput(child);
out:
	return err;
}

static int cas_store_write_to_tmp(struct cas_store *store, const void *buf,
				  size_t len, struct dentry **out_dentry);

static int write_compresssed_to_tmp(struct cas_store *store, const void *buf,
				    size_t len, struct dentry **dentry)
{
	void *dest;
	size_t dest_sz;
	int rc;

	rc = casfs_deflate(&dest, &dest_sz, buf, len);
	if (unlikely(rc != Z_OK))
		return rc;

	rc = cas_store_write_to_tmp(store, dest, dest_sz, dentry);
	kfree(dest);

	return rc;
}

static int cas_store_read_all(const struct path *objp, void **read_buf,
			      ssize_t *file_size);

static int read_compressed(const struct path *objp, void **read_buf,
			   ssize_t *file_size)
{
	void *buf;
	ssize_t sz;
	int err;

	err = cas_store_read_all(objp, &buf, &sz);
	if (unlikely(err))
		return err;

	*file_size = 0;
	err = casfs_inflate(read_buf, file_size, buf, sz);
	kfree(buf);

	return err;
}

static int cas_get_or_create_dir(struct path *parent, const char *child_name,
				 struct path *path)
{
	int err;

	err = cas_get_dir_child(parent, child_name, DCACHE_DIRECTORY_TYPE,
				path);
	if (likely(!err))
		return 0;
	if (err != -ENOENT)
		return err;

	err = cas_mkdir_dir(parent->dentry, child_name);
	if (err && err != -EEXIST)
		return err;

	return cas_get_dir_child(parent, child_name, DCACHE_DIRECTORY_TYPE,
				 path);
}

static int cas_store_init(struct path *parent, const char *dir,
			  struct cas_store *store, bool compressed,
			  struct cas_scratch *scratch)
{
	int err;
	int i = 0, n;
	char prefix[CAS_STORE_PREFIX_HEX_DIGITS + 1];

	prefix[CAS_STORE_PREFIX_HEX_DIGITS] = '\0';

	store->scratch = scratch;

	err = cas_get_or_create_dir(parent, dir, &store->top_dir);
	if (unlikely(err)) {
		cas_log_access_err(parent, dir, err);
		goto out;
	}
	for (; i < CAS_STORE_PREFIX_COUNT; i++) {
		n = i;
		for (int a = CAS_STORE_PREFIX_HEX_DIGITS - 1; a >= 0; a--) {
			prefix[a] = hex[n & 0xf];
			n >>= 4;
		}
		err = cas_get_or_create_dir(&store->top_dir, prefix,
					    &store->prefix_dirs[i]);
		if (unlikely(err)) {
			cas_log_access_err(&store->top_dir, prefix, err);
			goto out_free_paths;
		}
	}
	if (compressed) {
		store->write_to_tmp = write_compresssed_to_tmp;
		store->read_all = read_compressed;
	} else {
		store->write_to_tmp = cas_store_write_to_tmp;
		store->read_all = cas_store_read_all;
	}

	return 0;

out_free_paths:
	while (i--)
		path_put_init_(&store->prefix_dirs[i]);

	path_put_init_(&store->top_dir);
out:
	return err;
}

static void cas_store_destroy(struct cas_store *store)
{
	for (int i = 0; i < CAS_STORE_PREFIX_COUNT; i++)
		path_put_init_(&store->prefix_dirs[i]);

	path_put_init_(&store->top_dir);
}

static inline int get_prefix_from_hash(const struct cas_hash *hash)
{
	unsigned short *n = (unsigned short *)(hash->bytes);

	return cpu_to_be16(*n) >> (16 - 4 * CAS_STORE_PREFIX_HEX_DIGITS);
}

static inline struct path *cas_store_prefix_dir(struct cas_store *store,
						const struct cas_hash *hash)
{
	int i = get_prefix_from_hash(hash);

	return &store->prefix_dirs[i];
}

int cas_store_get_path(struct cas_store *store, const struct cas_hash *hash,
		       struct path *path)
{
	struct path *prefix;
	char hash_hex[CASFS_HASH_HEX_LEN + 1];

	hash_hex[CASFS_HASH_HEX_LEN] = 0;
	cas_hash_fill_hex(hash, hash_hex);
	prefix = cas_store_prefix_dir(store, hash);
	return cas_get_dir_child(prefix, hash_hex + CAS_STORE_PREFIX_HEX_DIGITS,
				 DCACHE_REGULAR_TYPE, path);
}

static int cas_store_get_meta_path(struct cas_store *store,
				   const struct cas_hash *hash,
				   const char *meta, struct path *path)
{
	int err;
	struct path *prefix;
	char *meta_path = __getname();

	if (unlikely(!meta_path))
		return -ENOMEM;

	snprintf(meta_path, PATH_MAX, "%20phN.%s", hash, meta);
	prefix = cas_store_prefix_dir(store, hash);
	err = cas_get_dir_child(prefix, meta_path + CAS_STORE_PREFIX_HEX_DIGITS,
				DCACHE_REGULAR_TYPE, path);
	__putname(meta_path);
	return err;
}

static int cas_store_read_all(const struct path *objp, void **read_buf,
			      ssize_t *file_size)
{
	int err = 0;
	struct file *file;
	loff_t size, pos;
	char *buf;
	ssize_t read, n;

	file = dentry_open(objp, O_RDONLY | O_LARGEFILE, current_cred());
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto out_put_objp;
	}

	size = i_size_read(d_inode(objp->dentry));

	buf = kmalloc(size, GFP_KERNEL);
	if (!buf) {
		err = -ENOMEM;
		goto out_close;
	}
	pos = read = 0;
	while (read < size) {
		n = kernel_read(file, buf + read, size - read, &pos);
		if (n <= 0) {
			err = n ?: -EIO;
			break;
		}
		read += n;
	}

	if (likely(!err)) {
		*file_size = size;
		*read_buf = buf;
	} else {
		kfree(buf);
	}

out_close:
	filp_close(file, NULL);
out_put_objp:
	path_put(objp);
	return err;
}

static int cas_store_get_bytes(struct cas_store *store,
			       const struct cas_hash *hash, void **read_buf,
			       ssize_t *file_size)
{
	int err;
	struct path objp;

	err = cas_store_get_path(store, hash, &objp);
	if (unlikely(err))
		return err;

	return store->read_all(&objp, read_buf, file_size);
}

static int cas_store_query(struct cas_store *store, const struct cas_hash *hash)
{
	int err;
	struct path objp;

	err = cas_store_get_path(store, hash, &objp);
	if (err)
		return err;

	path_put(&objp);
	return 0;
}
/* Requires size meta and tree is available if we want to support trees without
 * meta would need change here as well as cas_tree_open().
 */
static int cas_store_query_tree(struct cas_store *store,
				const struct cas_hash *hash)
{
	int err = 0;
	struct path objp;

	err = cas_store_query(store, hash);
	if (err != 0)
		return err;

	err = cas_store_get_meta_path(store, hash, CAS_TREE_META, &objp);
	if (err)
		return err;

	path_put(&objp);

	return 0;
}

static int cas_store_get_meta(struct cas_store *store,
			      const struct cas_hash *hash, const char *meta,
			      void **read_buf, ssize_t *file_size)
{
	int err;
	struct path objp;

	err = cas_store_get_meta_path(store, hash, meta, &objp);
	if (unlikely(err))
		return err;

	return cas_store_read_all(&objp, read_buf, file_size);
}

static inline int cas_store_rename(struct cas_store *store, struct dentry *old,
				   struct dentry *new)
{
	int err;
	struct inode *old_dir = d_inode(old->d_parent);
	struct inode *new_dir = d_inode(new->d_parent);

	struct renamedata rd = (struct renamedata){
		SET_MNT_IDMAPS(cas_store_ns(store)),
		SET_OLD_PARENT(old),
		.old_dentry = old,
		SET_NEW_PARENT(new),
		.new_dentry = new,
		.flags = RENAME_NOREPLACE,
	};

	if (old_dir == new_dir) {
		inode_lock_nested(old_dir, I_MUTEX_PARENT);
	} else if (old_dir < new_dir) {
		inode_lock_nested(old_dir, I_MUTEX_PARENT);
		inode_lock_nested(new_dir, I_MUTEX_PARENT2);
	} else {
		inode_lock_nested(new_dir, I_MUTEX_PARENT);
		inode_lock_nested(old_dir, I_MUTEX_PARENT2);
	}

	err = vfs_rename(&rd);

	if (old_dir == new_dir) {
		inode_unlock(old_dir);
	} else {
		inode_unlock(old_dir);
		inode_unlock(new_dir);
	}

	return err;
}

/*
 * Commits a validated, finalized scratch file dentry into its permanent
 * write-protected location under the given target dentry name.
 */
static int __cas_store_commit_scratch_file(struct cas_store *store,
					   struct dentry *dentry,
					   struct dentry *new_dentry)
{
	int err;
	struct inode *inode = d_inode(dentry);
	struct iattr newattrs;

	if (unlikely(d_is_positive(new_dentry))) {
		err = -EEXIST;
		goto out_put_dentry;
	}

	/*
	 * Strip write bits before committing to the store.
	 */
	inode_lock(inode);
	newattrs.ia_mode = inode->i_mode & ~0222;
	newattrs.ia_valid = ATTR_MODE;
	err = casfs_notify_change(cas_store_ns(store), dentry, &newattrs);
	inode_unlock(inode);

	if (unlikely(err)) {
		pr_err("casfs: failed to strip write permissions on scratch %pd: %d\n",
		       dentry, err);
		goto out_put_dentry;
	}

	err = cas_store_rename(store, dentry, new_dentry);

out_put_dentry:
	if (err == -EEXIST) {
		err = 0;
		cas_scratch_remove(store->scratch, dentry);
	}
	dput(new_dentry);
	return err;
}

int cas_store_put_file(struct cas_store *store, const struct cas_hash *hash,
		       struct dentry *dentry)
{
	struct path *prefix;
	struct dentry *new_dentry;
	char hash_hex[CASFS_HASH_HEX_LEN + 1];

	hash_hex[CASFS_HASH_HEX_LEN] = 0;
	cas_hash_fill_hex(hash, hash_hex);

	prefix = cas_store_prefix_dir(store, hash);

	new_dentry = casfs_lookup_one_unlocked(
		casfs_nop_idmap, hash_hex + CAS_STORE_PREFIX_HEX_DIGITS,
		prefix->dentry,
		CASFS_HASH_HEX_LEN - CAS_STORE_PREFIX_HEX_DIGITS);
	if (IS_ERR(new_dentry))
		return PTR_ERR(new_dentry);

	return __cas_store_commit_scratch_file(store, dentry, new_dentry);
}

static int cas_store_put_meta_file(struct cas_store *store,
				   const struct cas_hash *hash,
				   const char *meta, struct dentry *dentry)
{
	struct path *prefix;
	struct dentry *new_dentry;
	char meta_path[256];

	snprintf(meta_path, sizeof(meta_path), "%20phN.%s", hash, meta);
	prefix = cas_store_prefix_dir(store, hash);

	new_dentry = casfs_lookup_one_unlocked(
		casfs_nop_idmap, meta_path + CAS_STORE_PREFIX_HEX_DIGITS,
		prefix->dentry,
		strlen(meta_path) - CAS_STORE_PREFIX_HEX_DIGITS);
	if (IS_ERR(new_dentry))
		return PTR_ERR(new_dentry);

	return __cas_store_commit_scratch_file(store, dentry, new_dentry);
}

int cas_store_put_meta_tree_file(struct cas_store *store,
				 const struct cas_hash *hash,
				 struct dentry *dentry)
{
	return cas_store_put_meta_file(store, hash, CAS_TREE_META, dentry);
}

static int cas_store_write_to_tmp(struct cas_store *store, const void *buf,
				  size_t len, struct dentry **out_dentry)
{
	int err = 0, close_err;
	struct file *file;
	loff_t pos;
	ssize_t written, n;
	struct dentry *dentry;

	file = cas_scratch_create_file(store->scratch);
	if (IS_ERR(file))
		return PTR_ERR(file);

	for (pos = written = 0; written < len; written += n) {
		n = kernel_write(file, buf + written, len - written, &pos);
		if (n <= 0) {
			err = n ?: -EIO;
			break;
		}
	}

	dentry = dget(file_dentry(file));

	close_err = filp_close(file, NULL);
	if (likely(!err))
		err = close_err;

	if (unlikely(err)) {
		cas_scratch_remove(store->scratch, dentry);
		dput(dentry);
	} else {
		*out_dentry = dentry;
	}

	return err;
}

static int cas_store_put_meta(struct cas_store *store,
			      const struct cas_hash *hash, const char *meta,
			      const void *buf, size_t len)
{
	int err;
	struct dentry *dentry;

	err = cas_store_write_to_tmp(store, buf, len, &dentry);
	if (unlikely(err))
		return err;

	err = cas_store_put_meta_file(store, hash, meta, dentry);
	if (unlikely(err))
		cas_scratch_remove(store->scratch, dentry);

	dput(dentry);
	return err;
}

int cas_store_put_bytes(struct cas_store *store, const struct cas_hash *hash,
			const void *buf, size_t len)
{
	int err;
	struct dentry *dentry;

	err = store->write_to_tmp(store, buf, len, &dentry);
	if (unlikely(err))
		return err;

	err = cas_store_put_file(store, hash, dentry);
	if (unlikely(err))
		cas_scratch_remove(store->scratch, dentry);

	dput(dentry);
	return err;
}

int cas_store_put_tree(struct cas_store *store, const struct cas_hash *hash,
		       const void *buf, size_t len,
		       const struct cas_tree_meta *meta)
{
	int err;

	err = cas_store_put_bytes(store, hash, buf, len);
	if (unlikely(err))
		return err;

	err = cas_store_put_meta(store, hash, CAS_TREE_META, meta->sizes,
				 8 * meta->sizes_len);
	if (unlikely(err))
		return err;

	return err;
}

int cas_cache_init(const char *dir, struct cas_cache *cache,
		   const char *scratch_dir)
{
	int err;

	err = cas_get_dir_path(dir, &cache->root_path);
	if (unlikely(err))
		goto out;

	// Create scratch for writing files to before committing to
	// the backend store.  Sharing the scratch across all stores.
	err = cas_scratch_init(&cache->scratch, &cache->root_path, scratch_dir);
	if (unlikely(err))
		goto out;

	err = cas_store_init(&cache->root_path, CAS_BLOBS_DIR, &cache->blobs,
			     false, &cache->scratch);
	if (unlikely(err))
		goto out_put_path;

	err = cas_store_put_bytes(&cache->blobs, &cas_empty_file_hash, NULL, 0);
	if (unlikely(err))
		goto out_destroy_blobs;

	err = cas_store_init(&cache->root_path, CAS_OBJECTS_DIR,
			     &cache->objects, true, &cache->scratch);
	if (unlikely(err))
		goto out_destroy_blobs;

	err = cas_store_put_bytes(&cache->objects, &cas_empty_dir_hash,
				  cas_empty_dir_content,
				  cas_empty_dir_content_len);
	if (unlikely(err))
		goto out_destroy_objects;

	err = cas_store_put_meta(&cache->objects, &cas_empty_dir_hash,
				 CAS_TREE_META, NULL, 0);
	if (unlikely(err))
		goto out_destroy_objects;

	// Create ARI store where content will be found via SHA1(ARI) the
	// content will not have any additional compression and be directly
	// served to users.
	err = cas_store_init(&cache->root_path, CAS_ARIS_DIR, &cache->aris,
			     false, NULL);
	if (unlikely(err))
		goto out_destroy_objects;

	return 0;

out_destroy_objects:
	cas_store_destroy(&cache->objects);
out_destroy_blobs:
	cas_store_destroy(&cache->blobs);
out_put_path:
	path_put(&cache->root_path);
out:
	return err;
}

void cas_cache_destroy(struct cas_cache *cache)
{
	cas_store_destroy(&cache->blobs);
	cas_store_destroy(&cache->objects);
	cas_store_destroy(&cache->aris);
	cas_scratch_destroy(&cache->scratch);
	path_put(&cache->root_path);
}

int cas_cache_get_blob_bytes(struct cas_cache *cache,
			     const struct cas_hash *hash, void **read_buf,
			     ssize_t *file_size)
{
	return cas_store_get_bytes(&cache->blobs, hash, read_buf, file_size);
}

int cas_cache_get_tree_bytes(struct cas_cache *cache,
			     const struct cas_hash *hash, void **read_buf,
			     ssize_t *file_size, struct cas_tree_meta *meta)
{
	int err;
	void *buf;
	size_t len;

	err = cas_store_get_bytes(&cache->objects, hash, read_buf, file_size);
	if (unlikely(err))
		return err;

	err = cas_store_get_meta(&cache->objects, hash, CAS_TREE_META, &buf,
				 &len);
	if (unlikely(err)) {
		kfree(*read_buf);
		return err;
	}
	meta->sizes = (size_t *)buf;
	meta->sizes_len = len;
	return 0;
}

int cas_cache_get_object_bytes(struct cas_cache *cache,
			       const struct cas_hash *hash, void **read_buf,
			       ssize_t *file_size)
{
	return cas_store_get_bytes(&cache->objects, hash, read_buf, file_size);
}

int cas_cache_query_object(struct cas_cache *cache, const struct cas_hash *hash)
{
	return cas_store_query(&cache->objects, hash);
}
int cas_cache_query_tree(struct cas_cache *cache, const struct cas_hash *hash)
{
	return cas_store_query_tree(&cache->objects, hash);
}

int cas_cache_query_blob(struct cas_cache *cache, const struct cas_hash *hash)
{
	return cas_store_query(&cache->blobs, hash);
}

int cas_cache_object_path(struct cas_cache *cache, const struct cas_hash *hash,
			  struct path *path)
{
	return cas_store_get_path(&cache->objects, hash, path);
}
int cas_cache_get_meta_tree_path(struct cas_cache *cache,
				 const struct cas_hash *hash, struct path *path)
{
	return cas_store_get_meta_path(&cache->objects, hash, CAS_TREE_META,
				       path);
}
int cas_cache_blob_path(struct cas_cache *cache, const struct cas_hash *hash,
			struct path *path)
{
	return cas_store_get_path(&cache->blobs, hash, path);
}

int cas_cache_blob_size(struct cas_cache *cache, const struct cas_hash *hash,
			ssize_t *size)
{
	int err;
	struct path objp;

	err = cas_store_get_path(&cache->blobs, hash, &objp);
	if (unlikely(err))
		return err;

	*size = i_size_read(d_inode(objp.dentry));
	path_put(&objp);
	return 0;
}

// Create the scratch related objects, the dirname is used to create
// a new directory that is further divided up into inode related data
// this is shard into the dirs[] based on the inode number, at the
// top level we create any non-inode data
int cas_scratch_init(struct cas_scratch *scratch, struct path *parent,
		     const char *dirname)
{
	int i = 0;
	int err = 0;
	char bucket[4];
	struct path scratch_path;

	atomic64_set(&scratch->next_id, 0);
	err = cas_get_or_create_dir(parent, CAS_SCRATCH_DIR, &scratch_path);

	if (unlikely(err)) {
		cas_log_access_err(parent, CAS_SCRATCH_DIR, err);
		return err;
	}

	scratch->name = kstrdup(dirname, GFP_KERNEL);

	err = cas_mkdir_dir(scratch_path.dentry, scratch->name);

	if (err == -EEXIST)
		err = 0;

	if (unlikely(err)) {
		cas_log_access_err(&scratch_path, scratch->name, err);
		goto out_put_scratch;
	}

	err = cas_get_dir_child(&scratch_path, scratch->name,
				DCACHE_DIRECTORY_TYPE, &scratch->top);

	if (unlikely(err))
		goto out_put_scratch;

	// Create shard buckets for inodes
	for (; i <= CAS_STORE_SCRATCH_DIR_MASK; i++) {
		sprintf(bucket, "%d", i);
		err = cas_get_or_create_dir(&scratch->top, bucket,
					    &scratch->inodes[i]);
		if (unlikely(err)) {
			cas_log_access_err(&scratch->top, bucket, err);
			goto out_free;
		}
	}

	path_put(&scratch_path);

	return 0;
out_free:
	while (i--) {
		// Best effort to remove the scratch directories create
		casfs_vfs_rmdir(cas_scratch_ns(scratch), scratch->top.dentry,
				scratch->inodes[i].dentry);
		path_put_init_(&scratch->inodes[i]);
	}

	casfs_vfs_rmdir(cas_scratch_ns(scratch), scratch_path.dentry,
			scratch->top.dentry);
	path_put_init_(&scratch->top);
out_put_scratch:
	path_put(&scratch_path);
	kfree(scratch->name);
	return err;
}
void cas_scratch_destroy(struct cas_scratch *scratch)
{
	for (int i = 0; i <= CAS_STORE_SCRATCH_DIR_MASK; i++) {
		casfs_vfs_rmdir(cas_scratch_ns(scratch), scratch->top.dentry,
				scratch->inodes[i].dentry);
		path_put_init_(&scratch->inodes[i]);
	}
	casfs_vfs_rmdir(cas_scratch_ns(scratch), scratch->top.dentry->d_parent,
			scratch->top.dentry);

	path_put_init_(&scratch->top);

	kfree(scratch->name);
}
// Try to create the a new dentry for the input name in the input directory
// Returns dentry of existing or new dentry created or error
// If existing dentry is found exists output is set to true

static struct dentry *cas_scratch_try_create(struct cas_scratch *scratch,
					     struct dentry *dir,
					     const char *name, bool *exists)
{
	struct dentry *dentry = NULL;
	struct inode *dir_inode = NULL;
	const struct cred *old_cred;
	int err = 0;

	dir_inode = d_inode(dir);
	dentry = casfs_lookup_one_unlocked(casfs_nop_idmap, name, dir,
					   strlen(name));

	if (IS_ERR(dentry))
		return dentry;

	if (unlikely(d_really_is_positive(dentry))) {
		*exists = true;
		return dentry;
	}

	old_cred = cas_override_cred_from_inode(dir_inode);
	if (IS_ERR(old_cred)) {
		err = PTR_ERR(old_cred);
		goto out;
	}

	inode_lock_nested(dir_inode, I_MUTEX_PARENT);
	err = casfs_vfs_create(cas_scratch_ns(scratch), dir, dentry, 0644,
			       true);
	inode_unlock(dir_inode);

	cas_revert_cred(old_cred);

	/* Post-Creation Fixup Pass: Force 0644 metadata overwrite cleanly */
	if (err == 0) {
		struct iattr newattrs;

		*exists = false;
		newattrs.ia_mode = S_IFREG | 0644;
		newattrs.ia_valid = ATTR_MODE | ATTR_FORCE;

		inode_lock(d_inode(dentry));
		err = casfs_notify_change(cas_scratch_ns(scratch), dentry,
					  &newattrs);
		inode_unlock(d_inode(dentry));

	} else if (err == -EEXIST) {
		*exists = true;
		err = 0;
	}

out:
	if (err) {
		dput(dentry);
		return ERR_PTR(err);
	}
	return dentry;
}

// Create file based on inumber, index into our scratch directory and
// create the file.
struct dentry *cas_scratch_create_inum(struct cas_scratch *scratch,
				       unsigned long inum)
{
	char name[32];
	int bucket = inum & CAS_STORE_SCRATCH_DIR_MASK;
	struct dentry *dentry, *dir = scratch->inodes[bucket].dentry;
	bool exists = false;

	sprintf(name, "%ld", inum);
	dentry = cas_scratch_try_create(scratch, dir, name, &exists);

	return dentry;
}

struct dentry *cas_scratch_create(struct cas_scratch *scratch)
{
	char name[32];
	struct dentry *dentry;
	struct dentry *dir = scratch->top.dentry;
	bool exists = false;
	u64 id;

	id = atomic64_inc_return(&scratch->next_id);
	snprintf(name, sizeof(name), "tmp_%llu", id);

	dentry = cas_scratch_try_create(scratch, dir, name, &exists);
	if (IS_ERR(dentry))
		return dentry;

	return dentry;
}

// Remove a scratch file safely, regardless of who drops the final reference context
void cas_scratch_remove(struct cas_scratch *scratch, struct dentry *dentry)
{
	int err = 0;
	bool warn = false;
	struct inode *dir = d_inode(dentry->d_parent);
	const struct cred *old_cred;

	/*
	 * Elevate credentials using the parent directory's configuration.
	 * This guarantees that even if an arbitrary background daemon
	 * triggers the eviction pass, we possess the right to clear the scratch entry.
	 */
	old_cred = cas_override_cred_from_inode(dir);
	if (IS_ERR(old_cred)) {
		pr_err("casfs: failed to override credentials for scratch removal on %pd: %pe\n",
		       dentry, old_cred);
		return;
	}

	inode_lock_nested(dir, I_MUTEX_PARENT);

	if (d_inode(dentry)->i_nlink)
		err = vfs_unlink(cas_scratch_ns(scratch), dir, dentry, NULL);
	else
		warn = true;

	inode_unlock(dir);

	cas_revert_cred(old_cred);

	if (unlikely(err))
		pr_err("cannot remove temporarily created file %pd: %pe\n",
		       dentry, ERR_PTR(err));
	if (unlikely(warn))
		pr_err("warning temporary file %pd no longer exists\n", dentry);
}

static struct file *
cas_scratch_create_file_from_dentry(struct cas_scratch *scratch,
				    struct dentry *dentry)
{
	struct path path;
	struct file *file;
	int err;

	path.dentry = dentry;
	if (IS_ERR(path.dentry))
		return ERR_CAST(path.dentry);

	path.mnt = mntget(scratch->top.mnt);

	file = dentry_open(&path, O_RDWR | O_LARGEFILE, current_cred());
	if (IS_ERR(file)) {
		cas_scratch_remove(scratch, path.dentry);
		path_put(&path);
		return file;
	}

	err = vfs_truncate(&file->f_path, 0);
	if (err) {
		pr_err("Failed to truncate scratch file dentry %pd %pe\n",
		       dentry, ERR_PTR(err));

		// Release our reference to the opened file handle
		fput(file);

		// Clean up the scratch structure tracking for this dentry
		cas_scratch_remove(scratch, path.dentry);
		path_put(&path);

		return ERR_PTR(err);
	}

	path_put(&path);
	return file;
}
struct file *cas_scratch_create_file(struct cas_scratch *scratch)
{
	return cas_scratch_create_file_from_dentry(scratch,
						   cas_scratch_create(scratch));
}
struct file *cas_scratch_create_file_inum(struct cas_scratch *scratch,
					  unsigned long inum)
{
	return cas_scratch_create_file_from_dentry(
		scratch, cas_scratch_create_inum(scratch, inum));
}
