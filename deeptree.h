/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 Google LLC
 */

#ifndef _CASFS_DEEP_TREE_H_
#define _CASFS_DEEP_TREE_H_

#include <linux/fs.h>

#include "node.h"

struct cas_deep_entry {
	struct cas_hash hash;
	enum cas_object_type type;
	u64 size;
	char name[PATH_MAX];
};

/*
 * Read deeptree from provider.
 *    - cas_deeptree_open
 *    - cas_deeptree_next_entry
 *      ..
 *    - cas_deeptree_destroy
 */
struct cas_deeptree *cas_deeptree_open(struct cas_provider *provider,
				       const struct cas_hash *deep_tree_hash);
int cas_deeptree_next_entry(struct cas_deeptree *f, struct cas_deep_entry *e);
void cas_deeptree_destroy(struct cas_deeptree *f);

#endif /* _CASFS_DEEP_TREE_H_ */
