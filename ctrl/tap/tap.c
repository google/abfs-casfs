// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 */

#include "../../casfs.h"

#include "tap.h"
#include "ctrl.h"
#include "session.h"

/*
 * Tap the CAS filesystem access.
 *
 *  Most taps result in a direct translation to a tap_data structure
 * representing the action and added to the list for the client to
 * read.
 *
 *  Files opened for write are different in that the they are deferred
 * until the session is closed (or the file is deleted).  When deffering
 * the write file, an extra reference is inserted into the node to
 * prevent the file from being flushed to the blob store.
 *
 */

const char *tap_name(enum tap_trigger type)
{
	switch (type) {
	case TAP_MKDIR:
		return "mkdir";
	case TAP_SYMLINK:
		return "createlink";
	case TAP_LINK:
		return "link";
	case TAP_UNLINK:
		return "unlink";
	case TAP_RENAME:
		return "rename";
	case TAP_READDIR:
		return "lsdir";
	case TAP_SYMLINK_READ:
		return "readlink";
	case TAP_STAT:
		return "stat";
	case TAP_OPEN_READ:
		return "open";
	case TAP_OPEN_WRITE:
		return "create";
	default:
		return "";
	}
}

// Allocate the tap data object, if we have no space create mark
// session as overflow
static struct tap_data *tap_data_alloc_session(struct tap_ctrl *ctrl,
					       struct session *session)
{
	struct tap_data *out;

	if (atomic_read(&ctrl->allocated) >= casfs_max_tap_entries) {
		mutex_lock(&session->lock);
		if (!session->is_overflow) {
			session->is_overflow = true;
			atomic_inc(&ctrl->session_overflows);
			wake_up_interruptible(&session->wait);
		}
		mutex_unlock(&session->lock);
		return ERR_PTR(-ENOSPC);
	}

	out = tap_data_alloc(ctrl);
	if (!out)
		return ERR_PTR(-ENOMEM);

	return out;
}

/*
 * Allocate and initialize basic tap data, the hash,size,dirty attribute are
 * not populated here.
 */

static struct tap_data *tap_data_create(struct tap_ctrl *ctrl,
					struct session *session,
					enum tap_trigger type,
					struct cas_node *node, struct dentry *d,
					struct dentry *old_dentry)
{
	int rc = 0;
	struct tap_data *tap_data = tap_data_alloc_session(ctrl, session);

	if (IS_ERR(tap_data))
		return tap_data;

	tap_data->type = type;

	/* Populate the data from the node, this requires that the
	 * inode is locked
	 */
	tap_data->mode = node->inode.i_mode;

	rc = tap_data_add_path(tap_data, d);
	if (rc != 0)
		goto out_free;

	if (old_dentry) {
		rc = tap_data_add_aux_path(tap_data, old_dentry);
		if (rc != 0)
			goto out_free;
	}
	return tap_data;

out_free:
	tap_data_free(tap_data);

	return ERR_PTR(rc);
}
// Create the tap for the input type and add to our session
static void tap_simple(struct tap_ctrl *ctrl, enum tap_trigger type,
		       struct cas_node *node, struct dentry *d,
		       struct dentry *old_dentry)
{
	struct session *session = tap_session_get(ctrl, tap_session_id());
	struct tap_data *tap_data = NULL;

	if (!session)
		return;

	if (session->is_overflow)
		goto out;

	tap_data = tap_data_create(ctrl, session, type, node, d, old_dentry);
	if (IS_ERR(tap_data))
		goto out;

	// Get the dirty attribute and hash and size for the node
	tap_data->dirty = cas_node_is_dirty(node);
	tap_data->is_ari = false;

	if (!tap_data->dirty) {
		tap_data->is_ari = (node->backend_type == cas_type_ari);
		if (tap_data->is_ari) {
			if (node->ari)
				tap_data->ari = kstrdup(node->ari, GFP_KERNEL);
			else
				tap_data->ari = NULL;
			tap_data->size = i_size_read(CAS_TO_INODE(node));
		} else {
			cas_get_node_hash(node, &tap_data->hash,
					  &tap_data->size);
		}
	}
	// Clear out size for directories as this is not used
	if (cas_type_is_dir(node->type))
		tap_data->size = 0;
	tap_session_add(session, tap_data);

out:
	tap_session_put(session);
}

/* Taps */
void tap_mkdir(struct tap_ctrl *ctrl, struct cas_node *node, struct dentry *d)
{
	tap_simple(ctrl, TAP_MKDIR, node, d, NULL);
}
void tap_symlink(struct tap_ctrl *ctrl, struct cas_node *node, struct dentry *d,
		 const char *link)
{
	tap_simple(ctrl, TAP_SYMLINK, node, d, NULL);
}
void tap_link(struct tap_ctrl *ctrl, struct cas_node *node, struct dentry *s,
	      struct dentry *t)
{
	tap_simple(ctrl, TAP_LINK, node, t, s);
}
void tap_unlink(struct tap_ctrl *ctrl, struct cas_node *node, struct dentry *d)
{
	tap_simple(ctrl, TAP_UNLINK, node, d, NULL);
}

void tap_symlink_read(struct tap_ctrl *ctrl, struct cas_node *node,
		      struct dentry *d)
{
	tap_simple(ctrl, TAP_SYMLINK_READ, node, d, NULL);
}
void tap_getattr(struct tap_ctrl *ctrl, struct cas_node *node, struct dentry *d)
{
	/* Filter out directory stats */
	if (node->type != CAS_TYPE_DIR)
		tap_simple(ctrl, TAP_STAT, node, d, NULL);
}

void tap_readdir(struct tap_ctrl *ctrl, struct cas_node *node, struct file *f)
{
	// this will use the directory listing hash not the directory hash.
	struct tap_data *tap_data = NULL;
	struct session *session = tap_session_get(ctrl, tap_session_id());

	if (!session)
		return;

	if (session->is_overflow)
		goto out;

	tap_data = tap_data_create(ctrl, session, TAP_READDIR, node,
				   file_dentry(f), NULL);
	if (IS_ERR(tap_data))
		goto out;

	if (cas_dir_snapshot_list(node)) {
		tap_data_free(tap_data);
		goto out; // TODO add to session level failure
	}

	tap_data->hash = node->dir_list_hash;
	tap_data->size = 0;

	tap_session_add(session, tap_data);
out:
	tap_session_put(session);
}

void tap_rename(struct tap_ctrl *ctrl, struct cas_node *old_node,
		struct dentry *old, struct cas_node *new_node,
		struct dentry *new)
{
	tap_simple(ctrl, TAP_RENAME, old_node, new, old);
}

void tap_file_open(struct tap_ctrl *ctrl, struct cas_node *node, struct file *f,
		   bool read_only)
{
	tap_simple(ctrl, read_only ? TAP_OPEN_READ : TAP_OPEN_WRITE, node,
		   file_dentry(f), NULL);
}

void tap_attr_change(struct tap_ctrl *ctrl, struct cas_node *node,
		     struct dentry *d)
{
	if (!d_unhashed(d))
		tap_simple(ctrl, TAP_ATTR_CHANGE, node, d, NULL);
}
