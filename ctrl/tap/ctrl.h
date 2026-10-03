/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google LLC
 */
#ifndef _INC_TAP_CTRL_H
#define _INC_TAP_CTRL_H

#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/wait.h>

struct dentry;
struct kmem_cache;
struct cas_node;

#define TAP_SESSION_HASHTABLE_BITS 8

/*
 * Tap control is responsible for registering a directory (tap) for user
 * sessions to open temporary files against to start and stop tapping.
 * Taps are groups by the process group ID.
 *
 */
#define HISTO_BUCKETS 9
struct tap_ctrl {
	struct kmem_cache *cache;
	char cache_name[32];
	struct mutex lock;
	DECLARE_HASHTABLE(sessions,
			  TAP_SESSION_HASHTABLE_BITS); /* Active tap sessions */

	atomic64_t paths_total;
	atomic64_t paths_fast_stack;
	atomic64_t paths_slow_heap;
	u64 peak_depth_histogram[HISTO_BUCKETS];
	u64 peak_per_session_hwm;

	atomic_t allocated;
	atomic_t allocated_hwm;
	atomic_t session_overflows;
};

// Create and intitialize tap control structure a mount
extern struct tap_ctrl *tap_init(struct dentry *parent, int instance);
extern void tap_destroy(struct tap_ctrl *ctrl);

// Functions to generate taps
extern void tap_mkdir(struct tap_ctrl *ctrl, struct cas_node *node,
		      struct dentry *d);
extern void tap_symlink(struct tap_ctrl *ctrl, struct cas_node *node,
			struct dentry *d, const char *link);
extern void tap_link(struct tap_ctrl *ctrl, struct cas_node *node,
		     struct dentry *s, struct dentry *t);
extern void tap_unlink(struct tap_ctrl *ctrl, struct cas_node *node,
		       struct dentry *d);
extern void tap_rename(struct tap_ctrl *ctrl, struct cas_node *old_node,
		       struct dentry *old_dentry, struct cas_node *new_node,
		       struct dentry *new_dentry);

extern void tap_readdir(struct tap_ctrl *ctrl, struct cas_node *node,
			struct file *f);
extern void tap_symlink_read(struct tap_ctrl *ctrl, struct cas_node *node,
			     struct dentry *d);
extern void tap_getattr(struct tap_ctrl *ctrl, struct cas_node *node,
			struct dentry *d);
extern void tap_file_open(struct tap_ctrl *ctrl, struct cas_node *node,
			  struct file *f, bool read_only);
extern void tap_attr_change(struct tap_ctrl *ctrl, struct cas_node *node,
			    struct dentry *d);

extern void tap_free_node(struct tap_ctrl *ctrl, uint32_t sid,
			  struct cas_node *node);

// Allocate tap buffer
extern struct tap_data *tap_data_alloc(struct tap_ctrl *ctrl);
extern int tap_data_add_path(struct tap_data *data, struct dentry *d);
extern int tap_data_add_aux_path(struct tap_data *data, struct dentry *d);
extern void tap_data_free(struct tap_data *data);

#endif /* _TAP_CTRL_H */
