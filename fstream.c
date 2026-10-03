// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Google LLC
 */

#include <crypto/hash.h>
#include <linux/zlib.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>

#include "casfs.h"
#include "cas_zlib.h"
#include "node.h"
#include "fstream.h"
#include "hash.h"

#define ZLIB_BLOCK_SIZE 65535

struct cas_fstream {
	struct file *filp;
	loff_t f_offset;
	size_t f_len; // file length
	size_t d_len; // data length

	bool compressed;
	z_stream *zstream;
	u8 *scratch;

	struct cas_hash h;

	bool write;
	union {
		// Read data
		struct {
			u8 *chunk;
			int c_offset;
			int c_data_size;
		};
		// Write data
		struct {
			struct dentry *dentry;
			struct cas_store *store;

			// Hash write data
			bool hash;
			struct shash_desc *desc;
		};
	};
};

static int cas_parse_type(const char *buf, ssize_t size,
			  enum cas_object_type *type);

static struct cas_fstream *cas_fstream_alloc(bool compressed)
{
	struct cas_fstream *out = NULL;

	out = kzalloc(sizeof(*out), GFP_KERNEL);
	if (!out)
		return ERR_PTR(-ENOMEM);

	out->compressed = compressed;

	return out;
}

static int cas_fstream_add_hash(struct cas_fstream *stream)
{
	struct shash_desc *desc;
	int descsize;
	int err;

	descsize = crypto_shash_descsize(cas_sha1_tfm);
	desc = kmalloc(descsize + sizeof(*desc), GFP_KERNEL);
	if (unlikely(!desc))
		return -ENOMEM;

	desc->tfm = cas_sha1_tfm;

	err = crypto_shash_init(desc);
	if (unlikely(err)) {
		kfree(desc);
		return err;
	}

	stream->desc = desc;
	return 0;
}

void cas_fstream_destroy(struct cas_fstream *f)
{
	if (unlikely(!f))
		return;

	cas_fstream_close(f);
	if (f->write) {
		// Remove temp file
		if (f->dentry) {
			cas_store_remove_scratch(f->store, f->dentry);
			dput(f->dentry);
		}

		if (f->desc) {
			shash_desc_zero(f->desc);
			kfree(f->desc);
		}

		zlib_deflateEnd(f->zstream);
	} else {
		kfree(f->chunk);
		zlib_inflateEnd(f->zstream);
	}
	kfree(f->scratch);
	if (f->zstream) {
		vfree(f->zstream->workspace);
		kfree(f->zstream);
	}
	kfree(f);
}

static int cas_fstream_init_zlib_structures(struct cas_fstream *f)
{
	int err = 0;

	if (!f->compressed)
		return err;

	f->scratch = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (unlikely(!f->scratch))
		return -ENOMEM;

	f->zstream = kzalloc(sizeof(z_stream), GFP_KERNEL);
	if (unlikely(!f->zstream)) {
		err = -ENOMEM;
		goto out_free_scratch;
	}

	if (f->write) {
		f->zstream->workspace = vzalloc(
			zlib_deflate_workspacesize(MAX_WBITS, MAX_MEM_LEVEL));
		if (unlikely(!f->zstream->workspace)) {
			err = -ENOMEM;
			goto out_free_stream;
		}

		f->zstream->next_out = f->scratch;
		f->zstream->avail_out = PAGE_SIZE;
		err = zlib_deflateInit(f->zstream, Z_DEFAULT_COMPRESSION);
		if (unlikely(err))
			goto out_free_workspace;
	} else {
		f->zstream->workspace = vzalloc(zlib_inflate_workspacesize());
		if (unlikely(!f->zstream->workspace)) {
			err = -ENOMEM;
			goto out_free_stream;
		}

		f->zstream->next_in = f->scratch;
		err = zlib_inflateInit2(f->zstream, MAX_WBITS);
		if (unlikely(err))
			goto out_free_workspace;
	}

	return 0;

out_free_workspace:
	vfree(f->zstream->workspace);
	f->zstream->workspace = NULL;
out_free_stream:
	kfree(f->zstream);
	f->zstream = NULL;
out_free_scratch:
	kfree(f->scratch);
	f->scratch = NULL;

	return err;
}

static struct cas_fstream *cas_fstream_create(struct cas_store *store,
					      bool hash, bool compressed)
{
	int err = 0;
	struct file *filp = NULL;
	struct cas_fstream *stream = NULL;

	stream = cas_fstream_alloc(compressed);
	if (IS_ERR(stream))
		return stream;

	filp = cas_store_create_scratch_file(store);
	if (IS_ERR(filp)) {
		err = PTR_ERR(filp);
		goto out_free;
	}
	stream->write = true;
	stream->hash = hash;
	stream->filp = filp;
	stream->store = store;

	if (stream->hash) {
		err = cas_fstream_add_hash(stream);
		if (unlikely(err))
			goto out_free;
	}

	err = cas_fstream_init_zlib_structures(stream);
	if (unlikely(err))
		goto out_free;

	stream->dentry = dget(file_dentry(stream->filp));
	return stream;

out_free:
	cas_fstream_destroy(stream);
	return ERR_PTR(err);
}

struct cas_fstream *cas_fstream_create_uncompressed(struct cas_store *store,
						    bool hash)
{
	return cas_fstream_create(store, hash, false);
}

struct cas_fstream *cas_fstream_create_compressed(struct cas_store *store,
						  bool hash)
{
	return cas_fstream_create(store, hash, true);
}

struct cas_fstream *cas_fstream_path_open(const struct path *path,
					  bool compressed)
{
	int err = 0;
	struct file *filp = NULL;
	struct cas_fstream *stream = NULL;

	stream = cas_fstream_alloc(compressed);
	if (IS_ERR(stream))
		return stream;

	filp = dentry_open(path, O_RDONLY | O_LARGEFILE, current_cred());
	if (IS_ERR(filp)) {
		err = PTR_ERR(filp);
		goto out_free;
	}
	stream->chunk = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (unlikely(!stream->chunk)) {
		err = -ENOMEM;
		goto out_free;
	}

	stream->filp = filp;

	err = cas_fstream_init_zlib_structures(stream);
	if (unlikely(err))
		goto out_free;

	stream->f_len = i_size_read(d_inode(path->dentry));
	if (!stream->compressed)
		stream->d_len = stream->f_len;

	return stream;

out_free:
	cas_fstream_destroy(stream);
	return ERR_PTR(err);
}

struct cas_fstream *cas_fstream_open(struct cas_provider *provider,
				     const struct cas_hash *hash,
				     bool compressed)

{
	int err = 0;
	enum cas_req_obj_type t;
	struct path path;
	struct cas_fstream *stream = NULL;

	err = cas_provider_query(provider, hash, &t);
	if (unlikely(err))
		return ERR_PTR(err);

	err = cas_provider_path(provider, t, hash, &path);
	if (unlikely(err))
		return ERR_PTR(err);

	stream = cas_fstream_path_open(&path, compressed);
	if (IS_ERR(stream))
		goto out_put;

	memcpy(&stream->h, hash, sizeof(*hash));

out_put:
	path_put(&path);
	return stream;
}

int cas_fstream_commit(struct cas_fstream *f)
{
	int err = 0;

	if (!(f->write && f->dentry))
		return -EINVAL;

	// Move file into place
	err = cas_store_put_file(f->store, &f->h, f->dentry);
	if (unlikely(err))
		cas_store_remove_scratch(f->store, f->dentry);

	dput(f->dentry);
	f->dentry = NULL;

	return err;
}

int cas_fstream_commit_tree_meta(struct cas_fstream *f,
				 const struct cas_hash *hash)
{
	int err = 0;

	if (!(f->write && f->dentry))
		return -EINVAL;

	// Move file into place
	err = cas_store_put_meta_tree_file(f->store, hash, f->dentry);
	if (unlikely(err))
		cas_store_remove_scratch(f->store, f->dentry);

	dput(f->dentry);
	f->dentry = NULL;

	return err;
}

int cas_fstream_close(struct cas_fstream *f)
{
	int err = 0;
	int hash_err = 0;

	if (!f->filp)
		return 0;

	err = filp_close(f->filp, NULL);
	f->filp = NULL;

	if (f->write && f->hash && f->desc) {
		hash_err = crypto_shash_final(f->desc, f->h.bytes);
		shash_desc_zero(f->desc);
		kfree(f->desc);
		f->desc = NULL;
	}

	return err ? err : hash_err;
}

size_t cas_fstream_file_len(struct cas_fstream *f)
{
	return f->f_len;
}

size_t cas_fstream_data_len(struct cas_fstream *f)
{
	return f->d_len;
}

/*
 * Reads zlib compressed data from the fstream file and inflates it in PAGE_SIZE
 * chunks.
 */
static int cas_fstream_read_chunk_compressed(struct cas_fstream *f)
{
	int rc = Z_OK;

	f->c_data_size = 0;
	f->zstream->next_out = f->chunk;
	f->zstream->avail_out = PAGE_SIZE;

	for (;;) {
		rc = casfs_read_into_zstream(f->zstream, f->scratch, PAGE_SIZE,
					     f->filp, &f->f_offset);
		if (unlikely(rc))
			return rc;

		if (f->zstream->avail_in == 0)
			break;

		rc = casfs_inflate_stream(f->zstream);
		if (unlikely(rc))
			return rc;

		if (f->zstream->avail_out == 0)
			break;
	}

	if (f->zstream->avail_out == PAGE_SIZE)
		return -EIO;

	f->c_data_size = PAGE_SIZE - f->zstream->avail_out;
	f->c_offset = 0;
	return 0;
}

static int cas_fstream_read_chunk_direct(struct cas_fstream *f)
{
	int err = 0;
	int read = 0;
	ssize_t n = 0;

	f->c_data_size = 0;

	while (read < PAGE_SIZE) {
		n = kernel_read(f->filp, f->chunk + read, PAGE_SIZE - read,
				&f->f_offset);
		if (n <= 0) {
			if (n < 0)
				err = n;
			else
				err = read == 0 ? -EIO : 0;

			break;
		}
		read += n;
	}

	if (err)
		return err;

	f->c_data_size = read;
	f->c_offset = 0;
	return 0;
}

int cas_fstream_read_chunk(struct cas_fstream *f)
{
	if (f->compressed)
		return cas_fstream_read_chunk_compressed(f);

	return cas_fstream_read_chunk_direct(f);
}

int cas_fstream_read_byte(struct cas_fstream *f, u8 *data)
{
	int err = 0;

	if (f->c_offset == f->c_data_size) {
		err = cas_fstream_read_chunk(f);
		if (unlikely(err))
			return err;
	}
	*data = f->chunk[f->c_offset++];
	return 0;
}

int cas_fstream_read(struct cas_fstream *f, u8 *data, const size_t size,
		     const u8 *delim, bool inc_delim)

{
	int err = 0;
	int offset = 0;
	u8 val;

	while (offset < size) {
		err = cas_fstream_read_byte(f, &val);
		if (err)
			return err;

		if (delim && (val == *delim)) {
			if (inc_delim)
				data[offset] = val;
			return 0;
		}
		data[offset] = val;
		++offset;
	}

	if (delim)
		return -EINVAL;

	return 0;
}

int cas_fstream_read_varint(struct cas_fstream *f, u64 *out)
{
	u64 result = 0;
	int shift = 0;
	u8 byte;
	int n = 0;
	int err = 0;

	do {
		if (n >= 8)
			return -EINVAL;

		err = cas_fstream_read_byte(f, &byte);
		if (err)
			return err;

		n++;
		result |= (u64)(byte & 0x7F) << shift;
		shift += 7;
	} while (byte & 0x80);

	*out = result;
	return 0;
}

int cas_fstream_read_object_header(struct cas_fstream *f,
				   enum cas_object_type type)
{
	int err = 0;
	long long n;
	enum cas_object_type t;
	u8 delim = ' ';
	// [type]' '[size]
	char scratch[32] = { 0 };

	err = cas_fstream_read(f, scratch, sizeof(scratch), &delim, false);
	if (unlikely(err))
		return err;

	err = cas_parse_type(scratch, strlen(scratch), &t);
	if (unlikely(err))
		return err;

	if (t != type)
		return -EINVAL;

	delim = '\0';
	err = cas_fstream_read(f, scratch, sizeof(scratch), &delim, true);
	if (unlikely(err))
		return err;

	err = kstrtoll(scratch, 10, &n);
	if (unlikely(err))
		return err;

	if (!f->compressed && unlikely(f->c_offset + n != f->f_len))
		return -EINVAL;

	f->d_len = f->c_offset + n;

	return err;
}

static int cas_fstream_write_compressed(struct cas_fstream *f, const char *buf,
					int len)
{
	int rc = Z_OK;
	ssize_t written, written_bytes = 0;
	ssize_t prev_total_in, read = 0;
	ssize_t in_bytes, len_out;

	f->zstream->next_in = buf;
	f->zstream->avail_in = len;

	// Need to deflate until we've consumed all of the input buffer.
	for (;;) {
		f->zstream->next_out = f->scratch;
		f->zstream->avail_out = PAGE_SIZE;

		prev_total_in = f->zstream->total_in;
		rc = zlib_deflate(f->zstream, Z_NO_FLUSH);
		if (unlikely(rc != Z_OK)) {
			pr_err("zlib_deflate failed for file %pD\n", f->filp);
			return rc;
		}

		in_bytes = f->zstream->total_in - prev_total_in;
		f->d_len = f->zstream->total_in;

		// Update the hash using uncompressed data consumed by deflation.
		if (f->hash && f->desc && in_bytes > 0) {
			rc = crypto_shash_update(f->desc, buf + read, in_bytes);
			if (rc) {
				pr_err("crypto_shash_update failed for file %pD\n",
				       f->filp);
				return rc;
			}
		}
		read += in_bytes;

		len_out = PAGE_SIZE - f->zstream->avail_out;
		for (written = 0; written < len_out; written += written_bytes) {
			written_bytes =
				kernel_write(f->filp, f->scratch + written,
					     len_out - written, &f->f_offset);
			if (written_bytes <= 0) {
				rc = written_bytes ?: -EIO;
				pr_err("failed to write file %pD\n", f->filp);
				return rc;
			}

			f->f_len += written_bytes;
		}

		if (f->zstream->avail_in == 0)
			break;
	}

	return rc;
}

static int cas_fstream_write_direct(struct cas_fstream *f, const char *buf,
				    int len)
{
	int err = 0;
	ssize_t written, n;

	for (written = 0; written < len; written += n) {
		n = kernel_write(f->filp, buf + written, len - written,
				 &f->f_offset);
		if (n <= 0) {
			err = n ?: -EIO;
			break;
		}
		f->f_len += n;
		f->d_len += n;
		if (f->hash && f->desc) {
			err = crypto_shash_update(f->desc, buf + written, n);
			if (err)
				break;
		}
	}
	return err;
}

// Directly writing to file here
int cas_fstream_write(struct cas_fstream *f, const char *buf, int len)
{
	if (f->compressed)
		return cas_fstream_write_compressed(f, buf, len);

	return cas_fstream_write_direct(f, buf, len);
}

int cas_fstream_flush(struct cas_fstream *f)
{
	int rc = Z_OK;
	ssize_t written, n;
	ssize_t len_out = 0;

	if (!f->compressed)
		return 0;

	while (rc != Z_STREAM_END) {
		f->zstream->next_out = f->scratch;
		f->zstream->avail_out = PAGE_SIZE;

		rc = zlib_deflate(f->zstream, Z_FINISH);
		if (rc != Z_OK && rc != Z_STREAM_END)
			return rc;

		len_out = PAGE_SIZE - f->zstream->avail_out;
		for (written = 0; written < len_out; written += n) {
			n = kernel_write(f->filp, f->scratch + written,
					 len_out - written, &f->f_offset);
			if (n <= 0) {
				rc = n ?: -EIO;
				return rc;
			}
			f->f_len += n;
		}
	}

	return 0;
}

void cas_fstream_hash(struct cas_fstream *f, struct cas_hash *hash)
{
	memcpy(hash, &f->h, sizeof(*hash));
}

static int cas_parse_type(const char *buf, ssize_t size,
			  enum cas_object_type *type)
{
	if (size == 4 && !strncmp(buf, "tree", size)) {
		*type = CAS_TYPE_DIR;
		return 0;
	}
	if (size == 6 && !strncmp(buf, "commit", size)) {
		*type = CAS_TYPE_COMMIT;
		return 0;
	}
	if (size == 8 && !strncmp(buf, "deeptree", size)) {
		*type = CAS_TYPE_DEEPTREE;
		return 0;
	}
	pr_warn("unknown object type '%.*s'\n", (int)size, buf);
	return -EINVAL;
}

/* Get the size after zlib compression level 0 of a blob to init the inode */
size_t get_compressed_blob_size(size_t content_size)
{
	size_t uncompressed_size;
	size_t num_blocks;
	size_t zlib_overhead;
	int size_str_len;

	size_str_len = snprintf(NULL, 0, "%zu", content_size);
	uncompressed_size = 5 + size_str_len + 1 + content_size;

	if (uncompressed_size == 0)
		num_blocks = 1;
	else
		num_blocks = (uncompressed_size + ZLIB_BLOCK_SIZE - 1) /
			     ZLIB_BLOCK_SIZE;

	zlib_overhead = 2 + (num_blocks * 5) + 4;
	return uncompressed_size + zlib_overhead;
}
