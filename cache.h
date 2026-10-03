/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google LLC
 */

#ifndef _CACHE_H_
#define _CACHE_H_

#include <linux/fs.h>
#include <linux/fs_struct.h>
#include "hash.h"

struct cas_tree_meta {
	int sizes_len;
	size_t *sizes;
};

#define CAS_STORE_PREFIX_HEX_DIGITS 3
#define CAS_STORE_PREFIX_COUNT (1 << (4 * CAS_STORE_PREFIX_HEX_DIGITS))
#define CAS_STORE_SCRATCH_DIR_MASK 0xFF

struct cas_scratch {
	char *name;
	atomic64_t next_id;
	struct fs_struct *fs_struct;
	struct path top;
	struct path inodes[CAS_STORE_SCRATCH_DIR_MASK + 1];
};

// Initialize the scratch area under the input parent with directory name dirname
// this will fail if the dirname already exists as we want to start with a blank
// slate for now.
extern int cas_scratch_init(struct cas_scratch *scratch, struct path *parent,
			    const char *dirname);
extern void cas_scratch_destroy(struct cas_scratch *scratch);
extern struct dentry *cas_scratch_create(struct cas_scratch *scratch);
extern struct dentry *cas_scratch_create_inum(struct cas_scratch *scratch,
					      unsigned long inum);
extern void cas_scratch_remove(struct cas_scratch *store,
			       struct dentry *dentry);
extern struct file *cas_scratch_create_file(struct cas_scratch *scratch);
extern struct file *cas_scratch_create_file_inum(struct cas_scratch *scratch,
						 unsigned long inum);

struct cas_store {
	struct cred *cred;
	struct user_namespace *user_ns;
	struct path top_dir;
	struct path prefix_dirs[CAS_STORE_PREFIX_COUNT];
	struct cas_scratch *scratch;

	int (*write_to_tmp)(struct cas_store *store, const void *buf,
			    size_t len, struct dentry **out_dentry);
	int (*read_all)(const struct path *objp, void **read_buf,
			ssize_t *file_size);
};
static inline struct file *
cas_store_create_scratch_file(struct cas_store *store)
{
	return cas_scratch_create_file(store->scratch);
}
static inline struct file *
cas_store_create_scratch_file_inum(struct cas_store *store, unsigned long inum)
{
	return cas_scratch_create_file_inum(store->scratch, inum);
}
static inline void cas_store_remove_scratch(struct cas_store *store,
					    struct dentry *dentry)
{
	return cas_scratch_remove(store->scratch, dentry);
}
extern int cas_store_get_path(struct cas_store *store,
			      const struct cas_hash *hash, struct path *path);
extern int cas_store_put_bytes(struct cas_store *store,
			       const struct cas_hash *hash, const void *buf,
			       size_t len);
extern int cas_store_put_file(struct cas_store *store,
			      const struct cas_hash *hash,
			      struct dentry *dentry);
extern int cas_store_put_tree(struct cas_store *store,
			      const struct cas_hash *hash, const void *buf,
			      size_t len, const struct cas_tree_meta *meta);

extern int cas_store_put_meta_tree_file(struct cas_store *store,
					const struct cas_hash *hash,
					struct dentry *dentry);

struct cas_cache {
	struct cred *cred;
	struct user_namespace *user_ns;
	struct path root_path;
	struct cas_store blobs;
	struct cas_store objects;
	struct cas_store aris;
	struct cas_scratch scratch;
};

extern int cas_cache_init(const char *dir, struct cas_cache *cache,
			  const char *scratch_dir);
extern void cas_cache_destroy(struct cas_cache *cache);
extern int cas_cache_get_object_bytes(struct cas_cache *cache,
				      const struct cas_hash *hash,
				      void **read_buf, ssize_t *file_size);
extern int cas_cache_get_tree_bytes(struct cas_cache *cache,
				    const struct cas_hash *hash,
				    void **read_buf, ssize_t *file_size,
				    struct cas_tree_meta *meta);
extern int cas_cache_get_blob_bytes(struct cas_cache *cache,
				    const struct cas_hash *hash,
				    void **read_buf, ssize_t *file_size);
extern int cas_cache_query_object(struct cas_cache *cache,
				  const struct cas_hash *hash);
extern int cas_cache_query_tree(struct cas_cache *cache,
				const struct cas_hash *hash);
extern int cas_cache_query_blob(struct cas_cache *cache,
				const struct cas_hash *hash);
extern int cas_cache_object_path(struct cas_cache *cache,
				 const struct cas_hash *hash,
				 struct path *path);
extern int cas_cache_blob_path(struct cas_cache *cache,
			       const struct cas_hash *hash, struct path *path);
extern int cas_cache_blob_size(struct cas_cache *cache,
			       const struct cas_hash *hash, ssize_t *size);
extern int cas_cache_get_meta_tree_path(struct cas_cache *cache,
					const struct cas_hash *hash,
					struct path *path);
#endif /*_CACHE_H_*/
