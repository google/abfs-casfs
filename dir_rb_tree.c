// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 */

#include "dir_rb_tree.h"

static char *cas_new_rb_key(const char *name, int len,
			    enum cas_object_type type)
{
	char *key;

	/* TODO: Consider avoiding allocation for non-directory nodes. */
	key = kmalloc(len + 2, GFP_KERNEL);
	if (unlikely(!key))
		return NULL;
	memcpy(key, name, len);
	if (type == CAS_TYPE_DIR) { //FIXME what about commits?
		key[len] = '/';
		key[len + 1] = '\0';
	} else {
		key[len] = '\0';
	}
	return key;
}

static struct cas_rb_node *cas_new_rb_node(const char *name, int len,
					   const struct cas_node *node)
{
	struct cas_rb_node *rb_node;

	rb_node = kmalloc(sizeof(struct cas_rb_node), GFP_KERNEL);
	if (unlikely(!rb_node))
		return NULL;

	rb_node->rb_node = (struct rb_node){};
	rb_node->node = node;
	rb_node->name_len = len;
	rb_node->key = cas_new_rb_key(name, len, node->type);
	if (unlikely(!rb_node->key)) {
		kfree(rb_node);
		return NULL;
	}

	return rb_node;
}

static struct cas_rb_node *cas_search_rb_node(const struct rb_root *tree,
					      const char *key)
{
	struct rb_node *rb_node = tree->rb_node;
	struct cas_rb_node *n;
	int c;

	while (rb_node) {
		n = container_of(rb_node, struct cas_rb_node, rb_node);
		c = strcmp(key, n->key);
		if (c < 0)
			rb_node = rb_node->rb_left;
		else if (c > 0)
			rb_node = rb_node->rb_right;
		else
			return n;
	}
	return NULL;
}

/*
 * add_child_rb_node_to_tree() - Add given node to the rb_tree.
 * @tree:
 * @name:
 * @len:
 * @node:
 *
 * Returns: On success return 0.
 * -ENOMEM - Out of memory
 * -ESTALE - An item exists with the same key
 */
int cas_dir_add_rb_node(struct rb_root *tree, const char *name, int len,
			struct cas_node *node)
{
	struct rb_node **last = &tree->rb_node, *last_parent = NULL;
	struct cas_rb_node *new;
	struct cas_rb_node *n;
	int c;

	new = cas_new_rb_node(name, len, node);
	if (unlikely(!new))
		return -ENOMEM;

	while (*last) {
		last_parent = *last;
		n = rb_entry(*last, struct cas_rb_node, rb_node);
		c = strcmp(new->key, n->key);
		if (c < 0)
			last = &((*last)->rb_left);
		else if (c > 0)
			last = &((*last)->rb_right);
		else {
			kfree(new->key);
			kfree(new);
			return -ESTALE;
		}
	}
	rb_link_node(&new->rb_node, last_parent, last);
	rb_insert_color(&new->rb_node, tree);
	return 0;
}

static void cas_dir_free_rb_node(struct rb_node *rb_node)
{
	struct cas_rb_node *n;

	if (rb_node->rb_left)
		cas_dir_free_rb_node(rb_node->rb_left);

	if (rb_node->rb_right)
		cas_dir_free_rb_node(rb_node->rb_right);

	n = rb_entry(rb_node, struct cas_rb_node, rb_node);
	kfree(n->key);
	kfree(n);
}

/*
 * cas_dir_free_rb_nodes() - Free all the allocated rb_nodes of the tree.
 * @tree:
 */
void cas_dir_free_rb_nodes(struct rb_root *tree)
{
	if (likely(tree->rb_node))
		cas_dir_free_rb_node(tree->rb_node);
}

/*
 * cas_dir_remove_rb_node() - Remove and free up a tree node.
 * @tree:
 * @name:
 * @len:
 * @node:
 *
 * Returns: On success return 0.
 * -ENOMEM - out of memory
 * -ESTALE - if the specified node cannot be found
 */
int cas_dir_remove_rb_node(struct rb_root *tree, const char *name, int len,
			   struct cas_node *node)
{
	char *key;
	struct cas_rb_node *found;

	key = cas_new_rb_key(name, len, node->type);
	if (unlikely(!key))
		return -ENOMEM;

	found = cas_search_rb_node(tree, key);
	kfree(key);

	if (unlikely(!found))
		return -ESTALE;

	if (WARN_ON(found->node != node))
		pr_err("bad-bad node asked for %p got %p\n", node, found->node);

	WARN_ON(found->name_len != len);

	rb_erase(&found->rb_node, tree);

	kfree(found->key);
	kfree(found);

	return 0;
}
