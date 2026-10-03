// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 */

#include <linux/dcache.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/ctype.h>
#include <linux/zlib.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/semaphore.h>
#include <linux/cpumask.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>

#include "../../fs.h"
#include "../util.h"
#include "ctrl.h"
#include "../../cas_zlib.h"

/*
 * Helper to write inflate data to a file, the file can't be opened until
 * first chunk is inflated to determine if it is a blob or not.
 */
struct cas_object_inflate {
	struct cas_provider_file *file;
	const struct cas_hash *hash;
	struct cas_provider *provider;
	loff_t pos;
};

/* Stream, inflate/deflate is done via chunks of a fixed size */
struct git_stream {
	z_stream stream;
	void *cas_chunk;
	void *git_chunk;
};

/* Workspace wrapper for the concurrency pools */
struct git_stream_workspace {
	struct list_head list;
	struct git_stream stream;
};

struct git_ctrl {
	struct list_head deflate_pool;
	spinlock_t deflate_pool_lock;
	struct semaphore deflate_sem;

	struct list_head inflate_pool;
	spinlock_t inflate_pool_lock;
	struct semaphore inflate_sem;

	int pool_size;
};

static void git_stream_destroy(struct git_stream *stream, bool deflate)
{
	if (stream->stream.state) {
		if (deflate)
			zlib_deflateEnd(&stream->stream);
		else
			zlib_inflateEnd(&stream->stream);
	}

	if (stream->stream.workspace) {
		vfree(stream->stream.workspace);
		stream->stream.workspace = NULL;
	}
	vfree(stream->cas_chunk);
	vfree(stream->git_chunk);
	stream->cas_chunk = NULL;
	stream->git_chunk = NULL;
}

static int git_stream_init(struct git_stream *stream, bool deflate)
{
	int rc;

	if (deflate) {
		stream->stream.workspace = vzalloc(
			zlib_deflate_workspacesize(MAX_WBITS, MAX_MEM_LEVEL));
	} else {
		stream->stream.workspace =
			vzalloc(zlib_inflate_workspacesize());
	}
	if (!stream->stream.workspace)
		goto no_mem;

	stream->cas_chunk = vmalloc(PAGE_SIZE);
	if (!stream->cas_chunk)
		goto no_mem;

	stream->git_chunk = vmalloc(PAGE_SIZE);
	if (!stream->git_chunk)
		goto no_mem;

	/* Initialize zlib state once during pool creation */
	if (deflate)
		rc = zlib_deflateInit(&stream->stream, Z_NO_COMPRESSION);
	else
		rc = zlib_inflateInit2(&stream->stream, MAX_WBITS);

	if (rc != Z_OK)
		goto no_mem;

	return 0;

no_mem:
	git_stream_destroy(stream, deflate);
	return -ENOMEM;
}

struct git_ctrl *git_init(void)
{
	int i;
	struct git_stream_workspace *ws;
	struct git_ctrl *ctrl = kzalloc(sizeof(struct git_ctrl), GFP_KERNEL);

	if (unlikely(!ctrl))
		return NULL;

	ctrl->pool_size = casfs_max_git_pools;
	INIT_LIST_HEAD(&ctrl->deflate_pool);
	spin_lock_init(&ctrl->deflate_pool_lock);
	sema_init(&ctrl->deflate_sem, ctrl->pool_size);

	INIT_LIST_HEAD(&ctrl->inflate_pool);
	spin_lock_init(&ctrl->inflate_pool_lock);
	sema_init(&ctrl->inflate_sem, ctrl->pool_size);

	/* Initialize Deflate Pool */
	for (i = 0; i < ctrl->pool_size; i++) {
		ws = kzalloc(sizeof(*ws), GFP_KERNEL);
		if (!ws || git_stream_init(&ws->stream, true) != 0) {
			kfree(ws);
			goto no_mem;
		}
		list_add_tail(&ws->list, &ctrl->deflate_pool);
	}

	/* Initialize Inflate Pool */
	for (i = 0; i < ctrl->pool_size; i++) {
		ws = kzalloc(sizeof(*ws), GFP_KERNEL);
		if (!ws || git_stream_init(&ws->stream, false) != 0) {
			kfree(ws);
			goto no_mem;
		}
		list_add_tail(&ws->list, &ctrl->inflate_pool);
	}

	return ctrl;

no_mem:
	git_destroy(ctrl);
	return NULL;
}

void git_destroy(struct git_ctrl *ctrl)
{
	struct git_stream_workspace *ws, *tmp;

	if (!ctrl)
		return;

	/* Drain Deflate Pool */
	list_for_each_entry_safe(ws, tmp, &ctrl->deflate_pool, list) {
		list_del(&ws->list);
		git_stream_destroy(&ws->stream, true);
		kfree(ws);
	}

	/* Drain Inflate Pool */
	list_for_each_entry_safe(ws, tmp, &ctrl->inflate_pool, list) {
		list_del(&ws->list);
		git_stream_destroy(&ws->stream, false);
		kfree(ws);
	}

	kfree(ctrl);
}

/* Pool Helper: Get Workspace */
static struct git_stream_workspace *git_workspace_get(struct semaphore *sem,
						      spinlock_t *lock,
						      struct list_head *pool)
{
	struct git_stream_workspace *ws;

	if (down_killable(sem))
		return ERR_PTR(-EINTR);

	spin_lock(lock);
	ws = list_first_entry(pool, struct git_stream_workspace, list);
	list_del(&ws->list);
	spin_unlock(lock);

	return ws;
}

/* Pool Helper: Put Workspace */
static void git_workspace_put(struct git_stream_workspace *ws,
			      struct semaphore *sem, spinlock_t *lock,
			      struct list_head *pool)
{
	spin_lock(lock);
	list_add(&ws->list, pool);
	spin_unlock(lock);

	up(sem);
}

static loff_t cas_to_git_object_deflate(struct git_stream *deflate,
					bool is_blob, struct file *cas_file,
					struct file *git_file)
{
	int rc = 0;
	ssize_t chunk_size = 0;
	loff_t rd_pos = 0;
	loff_t wr_pos = 0;

	/* Fast reset of already initialized stream */
	rc = zlib_deflateReset(&deflate->stream);
	if (rc != Z_OK) {
		pr_err("Failed to reset zlib deflate stream (%d)\n", rc);
		goto out_err;
	}

	deflate->stream.total_in = 0;
	deflate->stream.total_out = 0;
	deflate->stream.next_out = deflate->git_chunk;
	deflate->stream.avail_out = PAGE_SIZE;

	if (is_blob) {
		int hdrLen = sprintf(deflate->cas_chunk, "blob %lld",
				     i_size_read(file_inode(cas_file)));
		deflate->stream.next_in = deflate->cas_chunk;
		deflate->stream.avail_in = hdrLen + 1; // Add 1 for NULL byte

		rc = zlib_deflate(&deflate->stream, Z_SYNC_FLUSH);
		if (rc != Z_OK) {
			pr_err("failed to deflate header (%d, %s)\n", rc,
			       deflate->stream.msg);
			goto out_err;
		}
		// Write header to git file
		rc = ctrl_kernel_write(git_file, deflate->git_chunk,
				       PAGE_SIZE - deflate->stream.avail_out,
				       &wr_pos);
		if (rc != 0)
			goto out_err;
	}

	deflate->stream.avail_in = 0;
	deflate->stream.next_out = deflate->git_chunk;
	deflate->stream.avail_out = PAGE_SIZE;

	for (;;) {
		if (deflate->stream.avail_in == 0) {
			chunk_size = kernel_read(cas_file, deflate->cas_chunk,
						 PAGE_SIZE, &rd_pos);
			if (chunk_size < 0) {
				rc = chunk_size;
				goto out_err;
			}
			deflate->stream.next_in = deflate->cas_chunk;
			deflate->stream.avail_in = chunk_size;

			// Break out before next deflate call to give
			// a chance to flush output buffer if needed when
			// out of data
		}
		if (deflate->stream.avail_out == 0) {
			// Flush out to file
			rc = ctrl_kernel_write(git_file, deflate->git_chunk,
					       PAGE_SIZE, &wr_pos);
			if (rc != 0)
				goto out_err;
			deflate->stream.avail_out = PAGE_SIZE;
			deflate->stream.next_out = deflate->git_chunk;
		}
		// Check if done
		if (deflate->stream.avail_in == 0)
			break;

		rc = zlib_deflate(&deflate->stream, Z_SYNC_FLUSH);
		if (rc != Z_OK) {
			pr_err("Failed to deflate (2) chunk (rc: %d, msg %s)\n",
			       rc, deflate->stream.msg);
			goto out_err;
		}
	}

	while ((rc = zlib_deflate(&deflate->stream, Z_FINISH)) == Z_OK) {
		if (deflate->stream.avail_out == 0) {
			rc = ctrl_kernel_write(git_file, deflate->git_chunk,
					       PAGE_SIZE, &wr_pos);
			if (rc != 0)
				goto out_err;
			deflate->stream.avail_out = PAGE_SIZE;
			deflate->stream.next_out = deflate->git_chunk;
		}
	}

	if (rc != Z_STREAM_END)
		goto out_err;

	if (deflate->stream.avail_out != PAGE_SIZE) {
		rc = ctrl_kernel_write(git_file, deflate->git_chunk,
				       PAGE_SIZE - deflate->stream.avail_out,
				       &wr_pos);
		if (rc != 0)
			goto out_err;
	}

	/* Note: zlib_deflateEnd() is removed so workspace remains ready for next use */
	return wr_pos;

out_err:
	pr_err("failed to deflate file (%d)\n", rc);
	return rc;
}

loff_t cas_to_git_object_write(struct git_ctrl *ctrl, bool is_blob,
			       struct file *cas_file, struct file *git_file)
{
	loff_t out = 0;
	struct git_stream_workspace *ws;

	ws = git_workspace_get(&ctrl->deflate_sem, &ctrl->deflate_pool_lock,
			       &ctrl->deflate_pool);
	if (IS_ERR(ws))
		return PTR_ERR(ws);

	out = cas_to_git_object_deflate(&ws->stream, is_blob, cas_file,
					git_file);

	git_workspace_put(ws, &ctrl->deflate_sem, &ctrl->deflate_pool_lock,
			  &ctrl->deflate_pool);

	return out;
}

// Helper function to open file based on first chunk of data
static int open_cas_file(struct cas_object_inflate *inflate, char *data,
			 ssize_t size, loff_t *wr_offset)
{
	static const char *blob_header = "blob ";
	static int blob_header_len = 5;

	int rc = 0;
	loff_t offset = 0;
	enum cas_req_obj_type git_type = cas_type_object;

	if (strncmp(data, blob_header, blob_header_len) == 0) {
		git_type = cas_type_blob;
		offset = blob_header_len;
		for (; offset < size; offset++) {
			if (*(data + offset) == 0)
				break;
		}

		// Increment offset past \\\0
		offset++;
	}
	if (offset == size) {
		pr_err("Found blob header but not offset\n");
		return -EINVAL;
	}

	inflate->file = cas_provider_new_file(inflate->provider, git_type);
	if (IS_ERR(inflate->file)) {
		pr_err("Failed to create new provider file for type %d\n",
		       (int)git_type);
		return PTR_ERR(inflate->file);
	}

	*wr_offset = offset;
	return rc;
}

static int cas_object_inflate_write(struct cas_object_inflate *inflate,
				    void *data, ssize_t size)
{
	int rc = 0;
	loff_t offset = 0;
	size_t count = size;

	if (inflate->pos == 0) {
		rc = open_cas_file(inflate, data, size, &offset);
		if (rc != 0)
			return rc;
	}
	count -= offset;
	// Flush out to file
	rc = cas_provider_file_write(inflate->file, data + offset, count,
				     &inflate->pos);

	if (rc < 0)
		return rc;

	return (rc == count) ? 0 : -EINVAL;
}

static int git_to_cas_object_inflate(struct git_stream *inflate,
				     struct cas_provider *provider,
				     struct file *git_file,
				     const struct cas_hash *hash)
{
	int rc = 0;
	ssize_t chunk_size = 0;
	loff_t rd_pos = 0;
	bool read_complete = false;
	struct cas_object_inflate obj = {
		.file = NULL, .hash = hash, .provider = provider, .pos = 0
	};

	/* Fast reset */
	rc = zlib_inflateReset(&inflate->stream);
	if (rc != Z_OK) {
		pr_err("Failed to reset zlib inflate stream (%d)\n", rc);
		goto out_err;
	}

	inflate->stream.total_in = 0;
	inflate->stream.total_out = 0;
	inflate->stream.avail_in = 0;
	inflate->stream.next_out = inflate->cas_chunk;
	inflate->stream.avail_out = PAGE_SIZE;

	for (;;) {
		if (!read_complete && inflate->stream.avail_in == 0) {
			// Read git chunk of data
			chunk_size = kernel_read(git_file, inflate->git_chunk,
						 PAGE_SIZE, &rd_pos);
			if (chunk_size < 0) {
				rc = chunk_size;
				goto out_err;
			}
			inflate->stream.next_in = inflate->git_chunk;
			inflate->stream.avail_in = chunk_size;
			read_complete = (chunk_size == 0);
		}
		if (inflate->stream.avail_out == 0) {
			rc = cas_object_inflate_write(&obj, inflate->cas_chunk,
						      PAGE_SIZE);
			if (rc != 0)
				goto out_err;

			inflate->stream.next_out = inflate->cas_chunk;
			inflate->stream.avail_out = PAGE_SIZE;
		}

		rc = zlib_inflate(&inflate->stream, Z_SYNC_FLUSH);
		if (rc != Z_OK)
			break;
	}

	if (rc != Z_STREAM_END) {
		pr_err("error with inflate\n");
		goto out_err;
	}

	if (inflate->stream.avail_out != PAGE_SIZE) {
		rc = cas_object_inflate_write(
			&obj, inflate->cas_chunk,
			PAGE_SIZE - inflate->stream.avail_out);
		if (rc != 0)
			goto out_err;
	}

	/* Note: zlib_inflateEnd() is removed so workspace remains ready for next use */
	if (obj.file)
		rc = cas_provider_file_close(obj.file, hash);

	return rc;

out_err:
	if (obj.file)
		rc = cas_provider_file_abort(obj.file);
	pr_err("failed to inflate file (%d)\n", rc);
	return rc;
}

int git_to_cas_object_write(struct git_ctrl *ctrl,
			    struct cas_provider *provider,
			    struct file *git_file, const struct cas_hash *hash)
{
	loff_t out = 0;
	struct git_stream_workspace *ws;

	ws = git_workspace_get(&ctrl->inflate_sem, &ctrl->inflate_pool_lock,
			       &ctrl->inflate_pool);
	if (IS_ERR(ws))
		return PTR_ERR(ws);

	out = git_to_cas_object_inflate(&ws->stream, provider, git_file, hash);

	git_workspace_put(ws, &ctrl->inflate_sem, &ctrl->inflate_pool_lock,
			  &ctrl->inflate_pool);

	return out;
}

int peek_git_type(struct git_ctrl *ctrl, struct file *git_file,
		  enum cas_req_obj_type *obj_type)
{
	int rc = 0;
	struct git_stream_workspace *ws;
	struct git_stream *inflate;
	loff_t offset = 0;
	char first_char, second_char;

	ws = git_workspace_get(&ctrl->inflate_sem, &ctrl->inflate_pool_lock,
			       &ctrl->inflate_pool);
	if (IS_ERR(ws))
		return PTR_ERR(ws);

	inflate = &ws->stream;

	/* Fast reset */
	rc = zlib_inflateReset(&inflate->stream);
	if (rc != Z_OK) {
		pr_err("Failed to reset zlib stream (%d)\n", rc);
		goto out_unlock;
	}

	inflate->stream.total_in = 0;
	inflate->stream.total_out = 0;
	inflate->stream.avail_in = 0;
	inflate->stream.next_out = inflate->cas_chunk;
	inflate->stream.next_in = inflate->git_chunk;
	// Require 2 bytes to disambiguate 'tr' (tree) vs 'ta' (tag)
	inflate->stream.avail_out = 2;

	for (;;) {
		rc = casfs_read_into_zstream(&inflate->stream,
					     inflate->git_chunk, PEEK_BUF_SIZE,
					     git_file, &offset);
		if (unlikely(rc))
			goto out_unlock;

		if (inflate->stream.avail_in == 0) {
			pr_err("cannot read from git file to determine type\n");
			rc = -EINVAL;
			goto out_unlock;
		}

		rc = casfs_inflate_stream(&inflate->stream);
		if (unlikely(rc))
			goto out_unlock;

		if (inflate->stream.avail_out == 0)
			break;
	}

	first_char = ((char *)inflate->cas_chunk)[0];
	second_char = ((char *)inflate->cas_chunk)[1];

	switch (first_char) {
	case 'b':
		*obj_type = cas_type_blob;
		break;
	case 'c':
		*obj_type = cas_type_object;
		break;
	case 't':
		if (second_char == 'r') {
			*obj_type = cas_type_tree;
		} else {
			*obj_type = cas_type_object;
		}
		break;
	default:
		pr_err("unable to peek at object type. prefix: %c%c\n",
		       first_char, second_char);
		rc = -EINVAL;
		break;
	}

out_unlock:
	git_workspace_put(ws, &ctrl->inflate_sem, &ctrl->inflate_pool_lock,
			  &ctrl->inflate_pool);
	return rc;
}
