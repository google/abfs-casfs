/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 Google LLC
 */

#ifndef _CAS_ZLIB_H
#define _CAS_ZLIB_H

#include <linux/zlib.h>

/*
 * casfs_deflate compresses the input source buffer with zlib default compression.
 * On a successful return, the caller owns freeing 'dest'.
 * On entry, 'dest_sz' value is ignored.
 * On exit, 'dest_sz' value is the size of compressed data.
 * Return value is one of ZLIB return codes.
 * For large datasets, a streaming method is preferred.
 */
extern int casfs_deflate(void **dest, size_t *dest_sz, const void *source,
			 size_t source_sz);

/*
 * casfs_inflate uncompresses the input source buffer in memory.
 * On a successful return, the caller owns freeing 'dest'.
 * On entry, a greater than zero value in '*dest_sz' will be used for the
 * initial output buffer size.
 * On exit, 'dest_sz' value is the size of uncompressed data.
 * Return value is one of ZLIB return codes.
 * For large datasets, a streaming method is preferred.
 */
extern int casfs_inflate(void **dest, size_t *dest_sz, const void *source,
			 size_t source_sz);

/*
 * casfs_read_into_zstream reads data from a file into the input buffer of the zstream.
 * If unconsumed data remains in the stream from a prior cycle, it is shifted
 * to the start of 'buf' via memmove before reading more data.
 */
extern int casfs_read_into_zstream(z_stream *stream, void *buf, size_t buf_size,
				   struct file *f, loff_t *f_offset);

/*
 * casfs_inflate_stream inflates a stream as long as progress can be made.
 * Return value is one of ZLIB return codes.
 */
extern int casfs_inflate_stream(z_stream *stream);

#endif
