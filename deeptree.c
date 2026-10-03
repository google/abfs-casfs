// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Google LLC
 */

#include <linux/fs.h>
#include <linux/namei.h>

#include "deeptree.h"
#include "fstream.h"
/*
 * Deeptree:
 *   Open file from cache
 *   Read file in chunks of PAGE_SIZE
 *   Parse out deep entries.
 */
struct cas_deeptree {
	struct cas_fstream *stream;
};

int cas_deeptree_next_entry(struct cas_deeptree *f, struct cas_deep_entry *e)
{
	int err = 0;
	u64 mode = 0;
	u8 delim = '\0';

	err = cas_fstream_read(f->stream, e->hash.bytes, CASFS_HASH_LEN, NULL,
			       false);
	if (err)
		return err;

	err = cas_fstream_read_varint(f->stream, &e->size);
	if (err)
		return err;

	err = cas_fstream_read_varint(f->stream, &mode);
	if (err)
		return err;

	err = cas_get_type_from_mode(mode, &e->type);
	if (err)
		return err;

	err = cas_fstream_read(f->stream, e->name, PATH_MAX, &delim, true);
	return err;
}

/*
 * Open deeptree from cache and validate header
 */
struct cas_deeptree *cas_deeptree_open(struct cas_provider *provider,
				       const struct cas_hash *deep_tree_hash)
{
	int err = 0;
	struct cas_deeptree *out = NULL;
	struct cas_fstream *stream = NULL;

	out = kzalloc(sizeof(*out), GFP_KERNEL);
	if (!out) {
		err = -ENOMEM;
		goto out_err;
	}

	stream = cas_fstream_open(provider, deep_tree_hash, true);
	if (IS_ERR(stream)) {
		err = PTR_ERR(stream);
		goto out_free;
	}

	out->stream = stream;
	err = cas_fstream_read_object_header(out->stream, CAS_TYPE_DEEPTREE);
	if (unlikely(err))
		goto out_free;

	return out;

out_free:
	cas_deeptree_destroy(out);
out_err:
	return ERR_PTR(err);
}

void cas_deeptree_destroy(struct cas_deeptree *f)
{
	if (unlikely(!f))
		return;

	cas_fstream_destroy(f->stream);
	kfree(f);
}
