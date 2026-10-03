/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google LLC
 */

#ifndef _CASFS_DIR_RB_TREE_H_
#define _CASFS_DIR_RB_TREE_H_

#include <linux/rbtree_types.h>
#include "node.h"

/*
 * The directory entries in the Git object model are stored as sorted by the
 * name of the entries. This name for the children that themselves are directory,
 * will have a trailing slash.
 * We use the kernel's rb_tree to store the names, in order to avoid the sorting
 * cost each time the directory needs to be hashed and stored.
 *
 * None of the below functions are thread-safe, so the necessary locks must be
 * acquired before calling them.
 */

struct cas_rb_node {
	/* Embedded rb_node to be used by rb_tree funcs. */
	struct rb_node rb_node;
	const struct cas_node *node;
	/* Name of child + a trailing slash if it is a directory. */
	const char *key;
	/* The length of the name excluding the trailing slash. */
	int name_len;
	// Status bit to include in snapshot
	char inc_in_tree;
};

static inline void cas_dir_init_rb_tree(struct rb_root *tree)
{
	*tree = RB_ROOT;
}

extern int cas_dir_add_rb_node(struct rb_root *tree, const char *name, int len,
			       struct cas_node *node);

extern int cas_dir_remove_rb_node(struct rb_root *tree, const char *name,
				  int len, struct cas_node *node);

extern void cas_dir_free_rb_nodes(struct rb_root *tree);

#endif /*_CASFS_DIR_RB_TREE_H_*/
