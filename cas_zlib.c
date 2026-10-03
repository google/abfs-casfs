// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Google LLC
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "cas_zlib.h"
#include "compat.h"

static inline int zlib_to_posix_err(int zlib_rc)
{
	switch (zlib_rc) {
	case Z_OK:
	case Z_STREAM_END:
		return 0;
	case Z_MEM_ERROR:
		return -ENOMEM;
	case Z_DATA_ERROR:
		return -EILSEQ; /* Standard POSIX error for bad/corrupted byte sequence */
	case Z_STREAM_ERROR:
		return -EINVAL; /* Invalid stream parameter or state */
	case Z_BUF_ERROR:
		return -EIO; /* Progress blocked, map to I/O error */
	default:
		return -EIO;
	}
}

int casfs_deflate(void **dest, size_t *dest_sz, const void *source,
		  size_t source_sz)
{
	int rc = 0;
	size_t sz;
	void *buf;
	z_stream stream;

	sz = deflateBound(source_sz);
	buf = kvmalloc(sz, GFP_KERNEL);
	if (unlikely(!buf))
		return -ENOMEM;

	memset(&stream, 0, sizeof(stream));
	stream.next_in = source;
	stream.avail_in = source_sz;
	stream.workspace =
		vzalloc(zlib_deflate_workspacesize(MAX_WBITS, MAX_MEM_LEVEL));

	if (unlikely(!stream.workspace)) {
		rc = -ENOMEM;
		goto out_free_buf;
	}

	stream.next_out = buf;
	stream.avail_out = sz;

	rc = zlib_deflateInit(&stream, Z_DEFAULT_COMPRESSION);
	if (unlikely(rc != Z_OK)) {
		rc = zlib_to_posix_err(rc);
		goto out_free_workspace;
	}

	// Z_FINISH to deflate in one-step since we're using deflateBound.
	rc = zlib_deflate(&stream, Z_FINISH);
	if (unlikely(rc != Z_STREAM_END)) {
		pr_err("deflate did not finish in a single step with source sz: %ld\n",
		       source_sz);
		rc = zlib_to_posix_err(rc != Z_OK ? rc : Z_DATA_ERROR);
		goto out_end;
	}

	// It is safe to ignore the return code for deflateEnd()
	// https://www.zlib.net/zlib_how.html
	zlib_deflateEnd(&stream);
	vfree(stream.workspace);
	*dest = buf;
	*dest_sz = stream.total_out;

	return 0;

out_end:
	zlib_deflateEnd(&stream);
out_free_workspace:
	vfree(stream.workspace);
out_free_buf:
	kvfree(buf);
	return rc;
}

int casfs_inflate(void **dest, size_t *dest_sz, const void *source,
		  size_t source_sz)
{
	z_stream stream;
	int rc;
	size_t buf_sz;
	void *buf;

	memset(&stream, 0, sizeof(stream));
	stream.next_in = source;
	stream.avail_in = source_sz;
	stream.workspace = vzalloc(zlib_inflate_workspacesize());
	if (unlikely(!stream.workspace))
		return -ENOMEM;

	rc = zlib_inflateInit2(&stream, MAX_WBITS);
	if (unlikely(rc != Z_OK)) {
		rc = zlib_to_posix_err(rc);
		goto out_free_workspace;
	}

	if (*dest_sz == 0)
		buf_sz = source_sz * 2;
	else
		buf_sz = *dest_sz;

	buf = kvmalloc(buf_sz, GFP_KERNEL);
	if (unlikely(!buf)) {
		rc = -ENOMEM;
		goto out_end;
	}

	stream.next_out = buf;
	stream.avail_out = buf_sz;

	for (;;) {
		rc = zlib_inflate(&stream, Z_NO_FLUSH);
		if (rc == Z_STREAM_END)
			break;

		if (unlikely(rc != Z_OK)) {
			rc = zlib_to_posix_err(rc);
			goto out_free_buf;
		}

		/* If we're out of room, double the size of our output buffer. */
		if (stream.avail_out == 0) {
			size_t next_buf_sz;
			void *next_buf;

			next_buf_sz = buf_sz * 2;
			next_buf = casfs_kvrealloc(buf, buf_sz, next_buf_sz,
						   GFP_KERNEL);
			if (unlikely(!next_buf)) {
				rc = -ENOMEM;
				goto out_free_buf;
			}

			stream.next_out = next_buf + buf_sz;
			stream.avail_out = buf_sz;
			buf = next_buf;
			buf_sz = next_buf_sz;
		}
	}

	zlib_inflateEnd(&stream);
	vfree(stream.workspace);
	*dest = buf;
	*dest_sz = stream.total_out;

	return zlib_to_posix_err(rc);

out_free_buf:
	kvfree(buf);
out_end:
	zlib_inflateEnd(&stream);
out_free_workspace:
	vfree(stream.workspace);
	return rc;
}

int casfs_read_into_zstream(z_stream *stream, void *buf, size_t buf_size,
			    struct file *f, loff_t *f_offset)
{
	size_t remaining;
	ssize_t n;

	if (unlikely(!buf || buf_size == 0))
		return -EINVAL;

	/*
	 * If unconsumed data remains from a previous invocation,
	 * shift it back to the beginning of the buffer.
	 */
	if (stream->avail_in > 0) {
		if (stream->next_in != buf)
			memmove(buf, stream->next_in, stream->avail_in);
	}
	stream->next_in = buf;

	/* Buffer is already full of unconsumed data */
	if (stream->avail_in >= buf_size)
		return 0;

	remaining = buf_size - stream->avail_in;

	while (remaining > 0) {
		n = kernel_read(f, (void *)buf + stream->avail_in, remaining,
				f_offset);
		if (n <= 0) {
			if (n < 0)
				return n;
			break;
		}
		stream->avail_in += n;
		remaining -= n;
	}

	return 0;
}

int casfs_inflate_stream(z_stream *stream)
{
	int rc = 0;

	for (;;) {
		rc = zlib_inflate(stream, Z_NO_FLUSH);
		if (rc == Z_STREAM_END)
			break;

		if (unlikely(rc != Z_OK))
			break;

		if (stream->avail_in == 0)
			break;

		if (stream->avail_out == 0)
			break;
	}

	return zlib_to_posix_err(rc);
}
