/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 Google LLC
 */

#ifndef _CASFS_FSTREAM_H_
#define _CASFS_FSTREAM_H_

#include <linux/fs.h>
#include <linux/crypto.h>
#include "provider.h"
#include "hash.h"

/* Create new file and hash written data (if hash==true)
 *   - cas_fstream_create  : create a new temporary file
 *   - cas_fstream_write   : write data to file (and hash)
 *   - cas_fstream_flush   : flush any pending data to file
 *   - ..
 *   - cas_fstream_close   : close file (and finalize hash)
 *   - cas_fstream_commit  : commit temp file to store with hash
 *   - cas_fstream_destroy : clean up
 */

struct cas_fstream *cas_fstream_create_uncompressed(struct cas_store *store,
						    bool hash);
struct cas_fstream *cas_fstream_create_compressed(struct cas_store *store,
						  bool hash);

/* Read file
 *    - cas_fstream_open    : open file
 *    - cas_fstream_read    : read data
 *    - cas_fstream_close   : close file
 *    - cas_fstream_destroy : clean up
 */
struct cas_fstream *cas_fstream_open(struct cas_provider *provider,
				     const struct cas_hash *hash,
				     bool compressed);
struct cas_fstream *cas_fstream_path_open(const struct path *path,
					  bool compressed);

int cas_fstream_close(struct cas_fstream *f);
void cas_fstream_destroy(struct cas_fstream *f);

int cas_fstream_write(struct cas_fstream *f, const char *buf, int len);
int cas_fstream_flush(struct cas_fstream *f);

int cas_fstream_read_chunk(struct cas_fstream *f);
int cas_fstream_read_byte(struct cas_fstream *f, u8 *data);
int cas_fstream_read(struct cas_fstream *f, u8 *data, const size_t size,
		     const u8 *delim, bool inc_delim);
int cas_fstream_read_varint(struct cas_fstream *f, u64 *out);
int cas_fstream_read_object_header(struct cas_fstream *f,
				   enum cas_object_type type);

size_t cas_fstream_file_len(struct cas_fstream *f);
size_t cas_fstream_data_len(struct cas_fstream *f);
void cas_fstream_hash(struct cas_fstream *f, struct cas_hash *hash);

int cas_fstream_commit(struct cas_fstream *f);
int cas_fstream_commit_tree_meta(struct cas_fstream *f,
				 const struct cas_hash *hash);

extern size_t get_compressed_blob_size(size_t content_size);

#endif /* _CASFS_FSTREAM_H_ */
