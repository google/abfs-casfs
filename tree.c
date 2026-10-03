// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Google LLC
 */
#include <linux/fs.h>
#include "casfs.h"
#include "tree.h"
#include "dir_rb_tree.h"
#include "fstream.h"

struct cas_tree {
	struct cas_fstream *git;
	struct cas_fstream *meta;
};

static inline int cas_get_mode_digits_nr(int mode);

static struct cas_tree *cas_tree_alloc(void)
{
	struct cas_tree *tree = NULL;

	tree = kzalloc(sizeof(*tree), GFP_KERNEL);
	if (!tree)
		return ERR_PTR(-ENOMEM);

	return tree;
}

struct cas_tree *cas_tree_create(struct cas_fs *fs)
{
	int err = 0;
	struct cas_tree *tree = NULL;
	struct cas_fstream *stream = NULL;
	struct cas_store *store = cas_fs_object_store(fs);

	tree = cas_tree_alloc();
	if (IS_ERR(tree))
		return tree;

	stream = cas_fstream_create_compressed(store, true);
	if (IS_ERR(stream)) {
		err = PTR_ERR(stream);
		goto out_free;
	}
	tree->git = stream;

	stream = cas_fstream_create_uncompressed(store, false);
	if (IS_ERR(stream)) {
		err = PTR_ERR(stream);
		goto out_free;
	}
	tree->meta = stream;

	return tree;

out_free:
	cas_tree_destroy(tree);
	return ERR_PTR(err);
}

/* Open tree and meta file associated with the input hash */
struct cas_tree *cas_tree_open(struct cas_fs *fs, struct cas_hash *hash)
{
	int err = 0;
	struct cas_tree *tree = NULL;
	struct path path;
	struct path meta_path;
	struct cas_fstream *stream = NULL;

	tree = cas_tree_alloc();
	if (IS_ERR(tree))
		return tree;

	err = cas_provider_query_tree(&fs->provider, hash);
	if (unlikely(err))
		goto out_free;

	err = cas_provider_tree_paths(&fs->provider, hash, &path, &meta_path);
	if (unlikely(err))
		goto out_free;

	stream = cas_fstream_path_open(&path, true);
	if (IS_ERR(stream)) {
		err = PTR_ERR(stream);
		goto out_path_put;
	}
	tree->git = stream;

	stream = cas_fstream_path_open(&meta_path, false);
	if (IS_ERR(stream)) {
		err = PTR_ERR(stream);
		goto out_path_put;
	}
	tree->meta = stream;

	err = cas_fstream_read_object_header(tree->git, CAS_TYPE_DIR);
	if (unlikely(err))
		goto out_path_put;

	path_put(&path);
	path_put(&meta_path);
	return tree;

out_path_put:
	path_put(&path);
	path_put(&meta_path);
out_free:
	cas_tree_destroy(tree);
	return ERR_PTR(err);
}

void cas_tree_destroy(struct cas_tree *tree)
{
	if (unlikely(!tree))
		return;

	cas_fstream_destroy(tree->git);
	cas_fstream_destroy(tree->meta);

	kfree(tree);
}
// When encoding a tree commits will be represented by a directory entry
static int cas_tree_encode_mode_from_type(enum cas_object_type type, int *mode)
{
	int err = 0;

	if (type == CAS_TYPE_COMMIT)
		*mode = CAS_ENT_MODE_DIR;
	else
		err = cas_get_mode_from_type(type, mode);

	return err;
}

int cas_tree_encode(struct cas_tree *tree, struct cas_node *node)
{
	int err = 0;
	struct cas_rb_node *n;
	ssize_t p, size = 0;
	int d, mode, sizes_len = 0;
	int64_t hash_size;
	char scratch[32];
	struct rb_node *rb_node;

	for (rb_node = rb_first(&node->entries); rb_node;
	     rb_node = rb_next(rb_node)) {
		n = rb_entry(rb_node, struct cas_rb_node, rb_node);
		// Snapshot if we are including this node so that we can
		// check below in case the state changes
		n->inc_in_tree = !n->node->is_excluded;
		if (!n->inc_in_tree)
			continue;
		if (unlikely(cas_tree_encode_mode_from_type(n->node->type,
							    &mode)))
			return -EINVAL;

		d = cas_get_mode_digits_nr(mode);
		if (unlikely(d < 0))
			return -EINVAL;
		size += CASFS_HASH_LEN + n->name_len + d +
			2 /* space and zero */;

		sizes_len++;
	}

	p = sprintf(scratch, "tree %ld", size);
	err = cas_fstream_write(tree->git, scratch, p + 1);
	if (unlikely(err))
		goto out;

	for (rb_node = rb_first(&node->entries); rb_node;
	     rb_node = rb_next(rb_node)) {
		n = rb_entry(rb_node, struct cas_rb_node, rb_node);
		if (!n->inc_in_tree)
			continue;

		cas_tree_encode_mode_from_type(n->node->type, &mode);
		p = sprintf(scratch, "%o ", mode);
		err = cas_fstream_write(tree->git, scratch, p);
		if (unlikely(err))
			goto out;
		err = cas_fstream_write(tree->git, n->key, n->name_len);
		if (unlikely(err))
			goto out;
		scratch[0] = '\0';
		err = cas_snapshot_node((struct cas_node *)n->node,
					(struct cas_hash *)&scratch[1],
					&hash_size);
		if (unlikely(err))
			goto out;
		err = cas_fstream_write(tree->git, scratch, CASFS_HASH_LEN + 1);
		if (unlikely(err))
			goto out;

		hash_size = cpu_to_le64(hash_size);
		err = cas_fstream_write(tree->meta, (const char *)&hash_size,
					sizeof(hash_size));
		if (unlikely(err))
			goto out;
	}

	err = cas_fstream_flush(tree->git);
	if (unlikely(err))
		goto out;

	err = cas_fstream_flush(tree->meta);
	if (unlikely(err))
		goto out;

	err = cas_fstream_close(tree->git);
	if (unlikely(err))
		goto out;

	err = cas_fstream_close(tree->meta);
out:
	return err;
}
void cas_tree_hash(struct cas_tree *tree, struct cas_hash *hash)
{
	return cas_fstream_hash(tree->git, hash);
}

int cas_tree_commit(struct cas_tree *tree, struct cas_fs *fs)
{
	// Commit tree file, meta file and update provider
	int err = 0;
	struct cas_hash h;

	cas_tree_hash(tree, &h);

	err = cas_fstream_commit_tree_meta(tree->meta, &h);
	if (unlikely(err))
		return err;

	err = cas_fstream_commit(tree->git);
	if (unlikely(err))
		return err;

	// Notify of new hash
	cas_provider_queue_new_object(&fs->provider, &h, cas_type_object);
	return 0;
}

int cas_tree_len(struct cas_tree *tree)
{
	return cas_fstream_data_len(tree->git);
}

int cas_tree_next_entry(struct cas_tree *tree, struct cas_dir_entry *e)
{
	int err = 0;
	char tmp[8] = { 0 };
	int val;
	u8 delim = ' ';

	// Read next entry from file along with size from meta file
	//'mode<space>filename<\0>hash'
	err = cas_fstream_read(tree->git, tmp, 8, &delim, false);
	if (unlikely(err))
		return err;

	err = kstrtoint(tmp, 8, &val);
	if (unlikely(err))
		return err;

	err = cas_get_type_from_mode(val, &e->type);
	if (unlikely(err))
		return err;

	delim = '\0';
	err = cas_fstream_read(tree->git, e->name, NAME_MAX, &delim, true);
	if (unlikely(err))
		return err;

	err = cas_fstream_read(tree->git, e->hash.bytes, CASFS_HASH_LEN, NULL,
			       false);
	if (unlikely(err))
		return err;

	err = cas_fstream_read(tree->meta, (char *)&e->size, sizeof(uint64_t),
			       NULL, false);
	if (unlikely(err))
		return err;

	e->size = le64_to_cpu(e->size);
	return 0;
}

size_t cas_tree_entry_count(struct cas_tree *tree)
{
	return cas_fstream_data_len(tree->meta) / sizeof(int64_t);
}

static inline int cas_get_mode_digits_nr(int mode)
{
	switch (mode) {
	case CAS_ENT_MODE_REG:
	case CAS_ENT_MODE_REG_EXEC:
	case CAS_ENT_MODE_COMMIT:
	case CAS_ENT_MODE_SYMLINK:
	case CAS_ENT_MODE_FIFO:
	case CAS_ENT_MODE_SOCKET:
		return 6;
	case CAS_ENT_MODE_DIR:
		return 5;
	default:
		pr_err("unknown mode 0%o\n", mode);
		return -EINVAL;
	}
}
