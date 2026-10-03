/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 Google LLC
 */

#ifndef _BLOB_H_
#define _BLOB_H_

#include <linux/fs.h>
#include "hash.h"

enum cas_cache_state {
	CAS_CACHE_UNLOADED, /* Backing details unmapped */
	CAS_CACHE_RESIDENT, /* Host storage path discovered and actively pinned */
	CAS_CACHE_READ_ACTIVE, /* Ready to stream reads from the backend provider */
	CAS_CACHE_WRITE_ACTIVE, /* Promoted to local writable scratch space */
};

struct cas_blob {
	enum cas_cache_state cache_state;
	struct path path;
	atomic_t generation;
};

extern void cas_blob_init(struct cas_blob *blob);
extern int cas_blob_open(struct cas_blob *blob);
extern void cas_blob_sync_cache(struct cas_blob *blob);
extern void cas_blob_reset(struct cas_blob *blob);

extern int cas_blob_snapshot(struct cas_blob *blob, const loff_t exp_size,
			     struct cas_hash *hash, loff_t *size,
			     bool *updated);

extern int cas_blob_ensure_read(struct cas_blob *blob);
extern int cas_blob_ensure_write(struct cas_blob *blob);

extern void cas_blob_scratch_setup_external(struct cas_blob *blob,
					    const struct path *path,
					    loff_t size);

#endif /* _BLOB_H_ */
