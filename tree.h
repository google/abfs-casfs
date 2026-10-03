/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 Google LLC
 */
#ifndef _CASFS_TREE_H_
#define _CASFS_TREE_H_

#include "fs.h"
#include "node.h"

struct cas_dir_entry {
	char name[NAME_MAX + 1];
	enum cas_object_type type;
	struct cas_hash hash;
	int64_t size;
};

struct cas_tree *cas_tree_create(struct cas_fs *fs);
struct cas_tree *cas_tree_open(struct cas_fs *fs, struct cas_hash *hash);

void cas_tree_destroy(struct cas_tree *tree);

int cas_tree_encode(struct cas_tree *tree, struct cas_node *node);
int cas_tree_commit(struct cas_tree *tree, struct cas_fs *fs);
void cas_tree_hash(struct cas_tree *tree, struct cas_hash *hash);

int cas_tree_len(struct cas_tree *tree);

int cas_tree_next_entry(struct cas_tree *tree, struct cas_dir_entry *e);
size_t cas_tree_entry_count(struct cas_tree *tree);

#endif /* _CASFS_TREE_H_ */
