// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 */

#include <linux/fs.h>
#include <linux/xattr.h>
#include <linux/security.h>
#include <linux/cred.h>
#include <linux/module.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <linux/atomic.h>
#include <linux/ratelimit.h>
#include <linux/namei.h>
#include <linux/crypto.h>
#include <crypto/hash.h>
#include "dir_rb_tree.h"
#include "casfs.h"
#include "node.h"
#include "fs.h"
#include "cas_tap.h"
#include "deeptree.h"
#include "tree.h"
#include "compat_dir.h"
#include "compat_inode.h"

struct dir_snapshot_work_results {
	atomic_t errors;
};

// note: result object is shared between all workers.
struct dir_snapshot_work {
	struct work_struct work;
	struct dir_snapshot_work_results *res;
	struct list_head head;
};

static int cas_dir_recalc(struct dentry *dentry);
int cas_get_type_from_mode(int mode, enum cas_object_type *type)
{
	switch (mode) {
	case CAS_ENT_MODE_REG:
		*type = CAS_TYPE_REG;
		return 0;
	case CAS_ENT_MODE_REG_EXEC:
		*type = CAS_TYPE_REG_EXEC;
		return 0;
	case CAS_ENT_MODE_DIR:
		*type = CAS_TYPE_DIR;
		return 0;
	case CAS_ENT_MODE_COMMIT:
		*type = CAS_TYPE_COMMIT;
		return 0;
	case CAS_ENT_MODE_SYMLINK:
		*type = CAS_TYPE_SYMLINK;
		return 0;
	case CAS_ENT_MODE_FIFO:
		*type = CAS_TYPE_FIFO;
		return 0;
	case CAS_ENT_MODE_SOCKET:
		*type = CAS_TYPE_SOCKET;
		return 0;
	case CAS_ENT_MODE_DEEPTREE:
		*type = CAS_TYPE_DEEPTREE;
		return 0;
	default:
		return -EINVAL;
	}
}

int cas_get_mode_from_type(enum cas_object_type type, int *mode)
{
	switch (type) {
	case CAS_TYPE_REG:
		*mode = CAS_ENT_MODE_REG;
		return 0;
	case CAS_TYPE_REG_EXEC:
		*mode = CAS_ENT_MODE_REG_EXEC;
		return 0;
	case CAS_TYPE_DIR:
		*mode = CAS_ENT_MODE_DIR;
		return 0;
	case CAS_TYPE_COMMIT:
		*mode = CAS_ENT_MODE_COMMIT;
		return 0;
	case CAS_TYPE_SYMLINK:
		*mode = CAS_ENT_MODE_SYMLINK;
		return 0;
	case CAS_TYPE_FIFO:
		*mode = CAS_ENT_MODE_FIFO;
		return 0;
	case CAS_TYPE_SOCKET:
		*mode = CAS_ENT_MODE_SOCKET;
		return 0;
	default:
		pr_err("unknown type %d\n", type);
		return -EINVAL;
	}
}

static struct dentry *safe_add_child(struct dentry *dentry, char *name,
				     size_t len, enum cas_object_type type,
				     const struct cas_hash *hash, size_t size,
				     umode_t mode)
{
	int err, err2;
	struct cas_node *node = INODE_TO_CAS(dentry->d_inode);
	struct inode *ichild;
	struct dentry *dchild;

	ichild = cas_new_node(dentry->d_sb, type, hash, size, mode);
	if (IS_ERR(ichild))
		return ERR_CAST(ichild);

	err = cas_dir_add_rb_node(&node->entries, name, len,
				  INODE_TO_CAS(ichild));
	if (unlikely(err))
		goto out_put_ichild;

	dchild = d_alloc_name(dentry, name);
	if (unlikely(!dchild)) {
		err = -ENOMEM;
		goto out_remove_rb_node;
	}
	if (unlikely(d_is_positive(dchild))) {
		err = -ESTALE;
		pr_err("did not expect to find existing dentry for child %s of dir %20phN\n",
		       name, &node->hash);
		goto out_put_dchild;
	}

	casfs_d_instantiate_persistent(dchild, ichild);
	return dchild;

out_put_dchild:
	dput(dchild);
out_remove_rb_node:
	err2 = cas_dir_remove_rb_node(&node->entries, name, len,
				      INODE_TO_CAS(ichild));
	if (unlikely(err2))
		pr_err("failed to clean up rb_node for child %s of dir %20phN: %pe\n",
		       name, &node->hash, ERR_PTR(err2));
out_put_ichild:
	iput(ichild);
	return ERR_PTR(err);
}

static int cas_load_dir(struct cas_node *node, struct dentry *dentry)
{
	int err;
	struct inode *inode = CAS_TO_INODE(node);
	struct cas_fs *fs = SB_TO_FS(dentry->d_sb);
	struct cas_dir_entry entry;
	struct dentry *dchild;
	struct cas_tree *tree = NULL;
	size_t entry_count = 0;

	pr_debug("loading dir %llu %20phN\n", (unsigned long long)inode->i_ino,
		 &node->hash);
	if (node->is_loaded)
		pr_warn("dir %pd/%pd/%pd (%20phN) is already loaded\n",
			dentry->d_parent->d_parent, dentry->d_parent, dentry,
			&node->hash);
	WARN_ON(node->is_loaded);

	tree = cas_tree_open(fs, &node->hash);
	if (IS_ERR(tree)) {
		err = PTR_ERR(tree);
		if (err != -ERESTARTSYS)
			pr_err("failed to open tree file for hash %20phN : %pe\n",
			       &node->hash, ERR_PTR(err));
		goto out;
	}

	entry_count = cas_tree_entry_count(tree);
	for (int i = 0; i < entry_count; i++) {
		err = cas_tree_next_entry(tree, &entry);
		if (unlikely(err)) {
			pr_err("failed to read entry #%d of dir %20phN: %pe\n",
			       i, &node->hash, ERR_PTR(err));
			goto out_free_tree;
		}

		dchild = safe_add_child(dentry, (char *)&entry.name,
					strlen(entry.name), entry.type,
					&entry.hash, entry.size, 0);
		if (IS_ERR(dchild)) {
			err = PTR_ERR(dchild);
			if (err != -ESTALE)
				goto out_free_tree;
			err = 0;
		} else {
			dput(dchild);
		}
	}

	node->is_loaded = true;
	i_size_write(inode, cas_tree_len(tree));
	cas_tree_destroy(tree);

	return 0;

out_free_tree:
	cas_tree_destroy(tree);
out:
	if ((err != -ERESTARTSYS) && (err != EINTR))
		pr_err("error loading directory %pd (%20phN): %pe\n", dentry,
		       &node->hash, ERR_PTR(err));
	return err;
}

int cas_dir_ensure_loaded(struct dentry *dentry)
{
	struct cas_node *node = INODE_TO_CAS(d_inode(dentry));

	if (node->is_loaded)
		return 0;

	return cas_load_dir(node, dentry);
}

struct list_node {
	struct dentry *dentry;
	struct list_head list;
};

static void cas_free_list_node(struct list_node *node)
{
	dput(node->dentry);
	list_del(&node->list);
	kfree(node);
}
static void cas_free_list_nodes(struct list_head *head)
{
	struct list_node *node, *tmp;

	list_for_each_entry_safe(node, tmp, head, list)
		cas_free_list_node(node);
}

static int cas_list_node_push(struct dentry *dentry, struct list_head *head)
{
	struct list_node *node = kmalloc(sizeof(struct list_node), GFP_KERNEL);

	if (unlikely(!node))
		return -ENOMEM;

	node->dentry = dget(dentry);
	list_add(&node->list, head);
	return 0;
}

static bool cas_dir_dirty_child_locked(struct dentry *dentry)
{
	struct dentry *child;
	struct cas_node *child_node;

	cas_list_for_each_child(child, dentry) {
		if (d_really_is_negative(child))
			continue;

		child_node = INODE_TO_CAS(d_inode(child));

		if (!cas_type_is_dir(child_node->type))
			continue;
		if (child_node->is_excluded)
			continue;
		if (atomic_read(&child_node->dirtiness))
			return true;
	}

	return false;
}

static int cas_dirty_children_dirs(struct dentry *dentry,
				   struct list_head *head, bool *dirty)
{
	int err = 0;

	struct dentry *child;
	struct cas_node *child_node;

	if (!inode_trylock_shared(dentry->d_inode))
		return -EAGAIN;

	*dirty = false;

	cas_list_for_each_child(child, dentry) {
		if (d_really_is_negative(child))
			continue;

		child_node = INODE_TO_CAS(d_inode(child));

		if (!cas_type_is_dir(child_node->type))
			continue;
		if (child_node->is_excluded)
			continue;

		if (atomic_read(&child_node->dirtiness)) {
			err = cas_list_node_push(child, head);
			if (unlikely(err))
				goto out;
			*dirty = true;
		}
	}

out:
	inode_unlock_shared(dentry->d_inode);
	return err;
}

static void dir_snapshot_offload(struct work_struct *work)
{
	int err = 0;
	struct dir_snapshot_work *dir_work =
		container_of(work, struct dir_snapshot_work, work);
	struct list_node *node, *tmp;

	list_for_each_entry_safe(node, tmp, &dir_work->head, list) {
		// Try to recalculate the directory hash in our list, if
		// the directory contains a dirty sub-directory it will not be
		// hashed
		if (inode_trylock_shared(node->dentry->d_inode)) {
			if (!cas_dir_dirty_child_locked(node->dentry))
				err = cas_dir_recalc(node->dentry);

			inode_unlock_shared(node->dentry->d_inode);
			if (unlikely(err))
				goto out;
		}
		// Free item from list event if we didn't get lock, will pick up
		// next time around
		cas_free_list_node(node);
	}
out:
	if (err) {
		cas_free_list_nodes(&dir_work->head);
		atomic_inc(&dir_work->res->errors);
	}
	kfree(dir_work);
}
static struct dir_snapshot_work *
alloc_snapshot_work(struct dir_snapshot_work_results *res)
{
	struct dir_snapshot_work *out = kzalloc(sizeof(*out), GFP_KERNEL);

	if (unlikely(!out))
		return out;

	out->res = res;
	INIT_WORK(&out->work, dir_snapshot_offload);
	INIT_LIST_HEAD(&out->head);
	return out;
}
// Walk the dentry looking for dirty leaf directories.
// Add these to the input work queue to be resolved.
static int cas_get_dirty_leafs(struct dentry *dentry,
			       struct workqueue_struct *wq,
			       struct dir_snapshot_work_results *res,
			       int jobs_per_work)
{
	struct inode *inode = d_inode(dentry);
	struct cas_node *node = INODE_TO_CAS(inode);
	struct list_node *cur = NULL;
	struct list_head *top = NULL;
	struct list_head head;
	struct dir_snapshot_work *work = NULL;

	int err = 0;
	bool dirty = false;
	int resolve_count = 0;
	// Short cut out
	if (!atomic_read(&node->dirtiness))
		return 0;

	// Stack of nodes to visit
	INIT_LIST_HEAD(&head);

	err = cas_list_node_push(dentry, &head);
	if (unlikely(err))
		return err;

	while (!list_empty(&head)) {
		top = head.next;
		cur = list_entry(top, struct list_node, list);
		dentry = cur->dentry;

		dirty = false;

		err = cas_dirty_children_dirs(dentry, &head, &dirty);
		if (unlikely(err)) {
			// If we couldn't acquire the lock assume
			// that it can't be resolved
			if (err != -EAGAIN)
				goto out;

			dirty = true;
			err = 0;
		}

		if (!dirty) {
			list_del(top);

			if (work == NULL) {
				work = alloc_snapshot_work(res);
				if (unlikely(!work)) {
					err = -ENOMEM;
					goto out;
				}
			}

			list_add(&cur->list, &work->head);
			if ((++resolve_count % jobs_per_work) == 0) {
				queue_work(wq, &work->work);
				work = NULL;
			}
		} else {
			cas_free_list_node(cur);
		}
	}
	if (work)
		queue_work(wq, &work->work);
out:
	cas_free_list_nodes(&head);
	return err;
}

// Update the hashes for the input directory hierarchy
int cas_dir_snapshot(struct dentry *dentry)
{
	const int bailout = 5000;
	const int jobs_per_work = 64;

	int err = 0;
	int count = 0;
	struct workqueue_struct *work_queue;
	struct cas_node *node = INODE_TO_CAS(d_inode(dentry));
	struct dir_snapshot_work_results res;

	ktime_t t1, t2;
	ktime_t traverse_time = 0;
	ktime_t resolve_time = 0;

	// Check if anything to do before setting up workers
	if (!atomic_read(&node->dirtiness))
		return 0;

	atomic_set(&res.errors, 0);

	work_queue =
		alloc_workqueue("casfs.hasher", WQ_MEM_RECLAIM | WQ_UNBOUND, 1);
	if (unlikely(!work_queue))
		return -ENOMEM;

	// Resolve all leafs that contain no-dirty sub-directories
	// This is done in a loop to bubble up until all directories
	// are resolved
	// TODO: Optimize this traversal; e.g. re-check a resolved leaf's parent
	// directly instead of rescanning the whole tree on every pass.
	do {
		t1 = ktime_get();
		err = cas_get_dirty_leafs(dentry, work_queue, &res,
					  jobs_per_work);
		if (unlikely(err))
			goto out;
		t2 = ktime_get();
		traverse_time += (t2 - t1);

		flush_workqueue(work_queue);
		resolve_time += (ktime_get() - t2);
		// Check for errors
		if (atomic_read(&res.errors)) {
			err = -EIO;
			goto out;
		}
		++count;
		if (count > bailout) {
			err = -EBUSY;
			goto out;
		}
	} while (atomic_read(&node->dirtiness));

out:
	destroy_workqueue(work_queue);

	return err;
}

void cas_make_dirty(struct dentry *dentry)
{
	struct cas_node *node;

	for (;;) {
		node = INODE_TO_CAS(d_inode(dentry));

		atomic_inc(&node->dirtiness);

		if (IS_ROOT(dentry) || node->is_excluded)
			break;

		dentry = dentry->d_parent;
	}
}

void cas_dir_changed(struct dentry *dentry)
{
	cas_make_dirty(dentry);
}

static void cas_dir_notify_parent(struct dentry *dentry,
				  const struct cas_hash *hash)
{
	struct dentry *p = dentry->d_parent;

	if (IS_ROOT(dentry) || p->d_sb != dentry->d_sb) {
		cas_fs_root_dir_change(SB_TO_FS(dentry->d_sb), hash);
		return;
	}

	cas_dir_changed(p);
}

static int cas_dir_recalc(struct dentry *dentry)
{
	int err;
	int dirtiness;

	struct cas_hash hash;
	struct inode *inode;
	struct cas_fs *fs;
	struct cas_tree *tree = NULL;
	struct cas_node *node;

	WARN_ON(!dentry);
	inode = d_inode(dentry);
	fs = SB_TO_FS(dentry->d_sb);

	if (unlikely(dentry->d_flags & DCACHE_DISCONNECTED))
		return 0;

	node = INODE_TO_CAS(inode);
	WARN_ON(!node);

	dirtiness = atomic_read(&node->dirtiness);

	if (dirtiness == 0)
		return 0;

	tree = cas_tree_create(fs);
	if (IS_ERR(tree))
		return PTR_ERR(tree);

	cas_lock_node(inode);

	err = cas_tree_encode(tree, node);
	if (unlikely(err))
		goto out_free_tree;

	cas_tree_hash(tree, &hash);

	if (!cas_hash_cmp(&node->hash, &hash)) {
		err = 0;
		goto out_free_tree;
	}

	err = cas_tree_commit(tree, fs);
	if (unlikely(err)) {
		pr_err("failed to write tree %20phN: %pe\n", &hash,
		       ERR_PTR(err));
		goto out_free_tree;
	}
	i_size_write(d_inode(dentry), cas_tree_len(tree));
	cas_set_node_hash(node, &hash, 0);

out_free_tree:
	if (likely(!err))
		atomic_sub(dirtiness, &node->dirtiness);

	cas_tree_destroy(tree);

	cas_unlock_node(inode);
	return err;
}

int cas_dir_add_child(struct inode *dir, struct dentry *dentry,
		      struct inode *new_inode)
{
	int err;
	struct cas_node *dir_node = INODE_TO_CAS(dir);

	err = cas_dir_add_rb_node(&dir_node->entries, dentry->d_name.name,
				  dentry->d_name.len, INODE_TO_CAS(new_inode));
	if (unlikely(err))
		return err;

	casfs_d_instantiate_persistent(dentry, new_inode);
	cas_touch_mtime_ctime(dir);

	dir_node->valid_dir_list_hash = false;
	return 0;
}

int cas_force_dir_hash(struct dentry *dentry, const struct cas_hash *hash)
{
	int err = 0;
	bool touched = false;
	struct inode *inode = d_inode(dentry);
	struct cas_node *node = INODE_TO_CAS(inode);

	cas_lock_node(inode);

	if (unlikely(atomic_read(&node->dirtiness))) {
		pr_warn_ratelimited(
			"%s cannot set hash %20phN directory is dirty for %pd\n",
			__func__, hash, dentry);
		err = -EINVAL;
		goto out;
	}

	if (unlikely(!cas_hash_cmp(&node->hash, hash)))
		goto out;

	if (node->is_loaded && cas_hash_cmp(&node->hash, &cas_empty_dir_hash)) {
		err = -ENOTEMPTY;
		goto out;
	}

	cas_set_node_hash(node, hash, 0);
	node->is_loaded = false;
	node->valid_dir_list_hash = false;
	touched = true;
	atomic_set(&node->dirtiness, 0);
	cas_touch_mtime_ctime(inode);

out:
	cas_unlock_node(inode);

	if (likely(touched))
		cas_dir_notify_parent(dentry, hash);

	return err;
}

struct deeptree_item {
	struct dentry *dentry;
	struct list_head list;
	struct cas_hash hash;
};

static int cas_dir_deeptree_add(struct dentry *dentry,
				const struct cas_hash *hash,
				struct list_head *head)
{
	struct deeptree_item *node =
		kmalloc(sizeof(struct deeptree_item), GFP_KERNEL);

	if (unlikely(!node))
		return -ENOMEM;

	node->dentry = dget(dentry);
	node->hash = *hash;
	list_add_tail(&node->list, head);
	return 0;
}

static int cas_dir_set_deep_entries(struct dentry *dentry,
				    struct cas_deeptree *df,
				    struct cas_deep_entry *ent,
				    struct list_head *head)
{
	int err = 0;
	struct cas_node *node = INODE_TO_CAS(d_inode(dentry));
	ssize_t size;
	enum cas_object_type type;
	const struct cas_hash *hash;
	struct dentry *dchild;
	struct qstr qname;
	u64 entry_count = ent->size;

	cas_lock_node(d_inode(dentry));
	cas_set_node_hash(node, &ent->hash, 0);

	for (u64 i = 1; i < entry_count; i++) {
		err = cas_deeptree_next_entry(df, ent);
		if (err)
			goto out;

		type = ent->type;
		size = ent->size;
		hash = &ent->hash;
		if (type == CAS_TYPE_DEEPTREE) {
			type = CAS_TYPE_DIR;
			hash = &cas_empty_dir_hash;
		}
		if (type == CAS_TYPE_DIR)
			size = 0;

		dchild = safe_add_child(dentry, ent->name, strlen(ent->name),
					type, hash, size, 0);
		if (IS_ERR(dchild)) {
			err = PTR_ERR(dchild);
			if (err != -ESTALE)
				goto out;
			err = 0;
			dchild = NULL;
		}

		if (unlikely(!dchild) && (ent->type == CAS_TYPE_DEEPTREE ||
					  ent->type == CAS_TYPE_DIR)) {
			qname.name = ent->name;
			qname.hash_len = hashlen_string(dentry, ent->name);
			dchild = d_lookup(dentry, &qname);
			if (IS_ERR(dchild)) {
				err = PTR_ERR(dchild);
				goto out;
			}
			if (unlikely(!dchild)) {
				pr_err("%s cannot find existing dentry for %s from %pd\n",
				       __func__, ent->name, dentry);
				err = -ESTALE;
				goto out;
			}
		}

		if (ent->type == CAS_TYPE_DEEPTREE) {
			err = cas_dir_deeptree_add(dchild, &ent->hash, head);

			if (unlikely(err)) {
				dput(dchild);
				goto out;
			}
		}

		// note: ent object re-used in recursive call to deep_entries to avoid
		// allocating at each directory level and should not be used after here.
		if (ent->type == CAS_TYPE_DIR) {
			i += ent->size - 1;
			err = cas_dir_set_deep_entries(dchild, df, ent, head);
			if (unlikely(err)) {
				dput(dchild);
				goto out;
			}
		}
		if (likely(dchild))
			dput(dchild);
	}

out:
	if (likely(!err))
		node->is_loaded = true;
	cas_unlock_node(d_inode(dentry));

	return err;
}

int cas_dir_set_deeptree(struct dentry *dentry,
			 const struct cas_hash *deep_tree_hash)
{
	int err = 0;
	struct cas_deeptree *df = NULL;
	struct cas_deep_entry *ent = NULL;
	struct cas_fs *fs = SB_TO_FS(dentry->d_sb);
	struct list_head head;
	struct deeptree_item *cur = NULL;
	struct list_head *top = NULL;

	INIT_LIST_HEAD(&head);

	ent = kzalloc(sizeof(*ent), GFP_KERNEL);
	if (unlikely(!ent))
		return -ENOMEM;

	err = cas_dir_deeptree_add(dentry, deep_tree_hash, &head);
	if (unlikely(err))
		goto out_err;

	while (!list_empty(&head)) {
		top = head.next;
		cur = list_entry(top, struct deeptree_item, list);

		df = cas_deeptree_open(&fs->provider, &cur->hash);

		if (IS_ERR(df)) {
			err = PTR_ERR(df);
			goto out_err;
		}
		err = cas_deeptree_next_entry(df, ent);
		if (unlikely(err))
			goto out_err_free_df;

		err = cas_dir_set_deep_entries(cur->dentry, df, ent, &head);
		if (err)
			goto out_err_free_df;

		cas_deeptree_destroy(df);
		dput(cur->dentry);
		list_del(top);
		kfree(cur);
	}

	cas_touch_mtime_ctime(d_inode(dentry));

	kfree(ent);
	return 0;

out_err_free_df:
	cas_deeptree_destroy(df);
out_err:
	while (!list_empty(&head)) {
		top = head.next;
		cur = list_entry(top, struct deeptree_item, list);
		dput(cur->dentry);
		list_del(top);
		kfree(cur);
	}
	kfree(ent);
	return err;
}

int cas_create(struct mnt_idmap_type *mnt_userns, struct inode *dir,
	       struct dentry *dentry, umode_t mode, bool excl)
{
	int err;
	struct cas_node *dir_node = INODE_TO_CAS(dir);
	enum cas_object_type type;
	struct inode *new_inode;

	err = cas_fs_check(SB_TO_FS(dir->i_sb));
	if (unlikely(err))
		return err;

	pr_debug("%s (%llu,%20phN,\"%pd\")\n", __func__,
		 (unsigned long long)dir->i_ino, &dir_node->hash, dentry);

	type = (mode & 0100) ? CAS_TYPE_REG_EXEC : CAS_TYPE_REG;
	new_inode =
		cas_new_node(dentry->d_sb, type, &cas_empty_file_hash, 0, mode);
	if (unlikely(!new_inode)) {
		err = -ENOMEM;
		goto out_err;
	}
	INODE_TO_CAS(new_inode)->is_loaded = true;

	cas_lock_node(dir);
	err = cas_dir_ensure_loaded(dentry->d_parent);
	if (unlikely(err))
		goto out_unlock;

	err = cas_dir_add_child(dir, dentry, new_inode);
	if (unlikely(err))
		goto out_unlock;

	cas_unlock_node(dir);

	cas_dir_changed(dentry->d_parent);

	return 0;

out_unlock:
	cas_unlock_node(dir);
	iput(new_inode);
out_err:
	d_drop(dentry);
	return err;
}

struct dentry *cas_mkdir_internal(struct inode *dir, struct dentry *dentry,
				  enum cas_object_type type, umode_t mode)
{
	int err;
	struct cas_node *dir_node = INODE_TO_CAS(dir);
	struct inode *new_inode;

	pr_debug("%s (%llu,%20phN,\"%pd\")\n", __func__,
		 (unsigned long long)dir->i_ino, &dir_node->hash, dentry);

	new_inode =
		cas_new_node(dentry->d_sb, type, &cas_empty_dir_hash, 0, mode);
	if (unlikely(!new_inode)) {
		err = -ENOMEM;
		goto out_err;
	}

	i_size_write(new_inode, cas_empty_dir_content_len);
	INODE_TO_CAS(new_inode)->is_loaded = true;

	cas_lock_node(dir);
	err = cas_dir_ensure_loaded(dentry->d_parent);
	if (unlikely(err))
		goto out_unlock;

	err = cas_dir_add_child(dir, dentry, new_inode);
	if (unlikely(err))
		goto out_unlock;

	cas_tap_mkdir(dentry);
	cas_unlock_node(dir);

	cas_dir_changed(dentry->d_parent);

	return NULL;

out_unlock:
	cas_unlock_node(dir);
	iput(new_inode);
out_err:
	d_drop(dentry);
	return ERR_PTR(err);
}

struct dentry *cas_mkdir(struct mnt_idmap_type *mnt_userns, struct inode *dir,
			 struct dentry *dentry, umode_t mode)
{
	return cas_mkdir_internal(dir, dentry, CAS_TYPE_DIR, mode);
}

int cas_mknod(struct mnt_idmap_type *mnt_userns, struct inode *dir,
	      struct dentry *dentry, umode_t mode, dev_t rdev)
{
	int err = 0;
	struct cas_node *dir_node = INODE_TO_CAS(dir);
	enum cas_object_type type;
	struct inode *new_inode;

	err = cas_fs_check(SB_TO_FS(dir->i_sb));
	if (unlikely(err))
		return err;

	pr_debug("%s (%llu,%20phN,\"%pd\", %d, %d)\n", __func__,
		 (unsigned long long)dir->i_ino, &dir_node->hash, dentry, mode,
		 rdev);

	if (rdev) {
		err = -EOPNOTSUPP;
		goto out_err;
	}

	if (S_ISFIFO(mode))
		type = CAS_TYPE_FIFO;
	else if (S_ISSOCK(mode))
		type = CAS_TYPE_SOCKET;
	else {
		err = -EOPNOTSUPP;
		goto out_err;
	}

	new_inode = cas_new_node(dentry->d_sb, type, &cas_zero_hash, 0, mode);
	if (unlikely(!new_inode)) {
		err = -ENOMEM;
		goto out_err;
	}

	cas_lock_node(dir);
	err = cas_dir_ensure_loaded(dentry->d_parent);
	if (unlikely(err))
		goto out_unlock;

	err = cas_dir_add_child(dir, dentry, new_inode);
	if (unlikely(err))
		goto out_unlock;

	cas_unlock_node(dir);

	/*
	 * mknod types do not contribute to the parent directory's
	 * hash so don't need cas_dir_changed here
	 */
	return 0;

out_unlock:
	cas_unlock_node(dir);
	iput(new_inode);
out_err:
	d_drop(dentry);
	return err;
}

int cas_symlink(struct mnt_idmap_type *mnt_userns, struct inode *dir,
		struct dentry *dentry, const char *link)
{
	int err;
	struct cas_node *dir_node = INODE_TO_CAS(dir);
	struct cas_fs *fs = SB_TO_FS(dir->i_sb);
	struct cas_hash hash;
	struct inode *new_inode;

	err = cas_fs_check(SB_TO_FS(dir->i_sb));
	if (unlikely(err))
		return err;

	pr_debug("%s (%llu,%20phN,\"%pd\", \"%s\")\n", __func__,
		 (unsigned long long)dir->i_ino, &dir_node->hash, dentry, link);

	err = cas_hash_blob(link, strlen(link), &hash);
	if (unlikely(err))
		goto out_err;

	err = cas_fs_new_blob(fs, &hash, link, strlen(link));
	if (unlikely(err))
		goto out_err;

	new_inode = cas_new_node(dentry->d_sb, CAS_TYPE_SYMLINK, &hash,
				 strlen(link), 0);
	if (unlikely(!new_inode)) {
		err = -ENOMEM;
		goto out_err;
	}

	cas_lock_node(dir);
	WARN_ON(dir != dentry->d_parent->d_inode);
	err = cas_dir_ensure_loaded(dentry->d_parent);
	if (unlikely(err))
		goto out_unlock;

	err = cas_dir_add_child(dir, dentry, new_inode);
	if (unlikely(err))
		goto out_unlock;

	cas_load_symlink(d_inode(dentry));

	cas_tap_symlink(dentry, link);
	cas_unlock_node(dir);

	cas_dir_changed(dentry->d_parent);

	return 0;

out_unlock:
	cas_unlock_node(dir);
	iput(new_inode);
out_err:
	d_drop(dentry);
	return err;
}

int cas_link(struct dentry *source, struct inode *dir, struct dentry *dentry)
{
	int err;
	struct inode *inode = d_inode(source);
	struct cas_node *dir_node = INODE_TO_CAS(dir);

	err = cas_fs_check(SB_TO_FS(dir->i_sb));
	if (unlikely(err))
		return err;

	pr_debug("%s %llu\n", __func__,
		 (unsigned long long)source->d_inode->i_ino);

	cas_lock_node(dir);

	err = cas_dir_ensure_loaded(dentry->d_parent);
	if (unlikely(err))
		goto out_err_unlock;

	err = cas_dir_add_rb_node(&dir_node->entries, dentry->d_name.name,
				  dentry->d_name.len, INODE_TO_CAS(inode));
	if (unlikely(err))
		goto out_err_unlock;

	err = simple_link(source, dir, dentry);
	if (unlikely(err)) {
		cas_dir_remove_rb_node(&dir_node->entries, dentry->d_name.name,
				       dentry->d_name.len, INODE_TO_CAS(inode));
		goto out_err_unlock;
	}
	cas_touch_mtime_ctime(dir);

	/* Target inode link count changed: touch ctime */
	casfs_inode_set_ctime_current(inode);
	mark_inode_dirty(inode);

	cas_tap_link(source, dentry);

	cas_unlock_node(dir);
	return 0;

out_err_unlock:
	cas_unlock_node(dir);
	return err;
}

static int cas_remove(const char *op, struct inode *dir, struct dentry *dentry)
{
	int err;
	struct cas_node *dir_node = INODE_TO_CAS(dir);
	struct dentry *dir_ent = dentry->d_parent;
	struct cas_node *node = INODE_TO_CAS(d_inode(dentry));
	struct timespec64 ts;

	pr_debug("%s (%llu,%20phN,\"%pd\")\n", op,
		 (unsigned long long)dir->i_ino, &dir_node->hash, dentry);

	if (node->type == CAS_TYPE_CTRL)
		return simple_rmdir(dir, dentry);

	cas_lock_node(dir);
	WARN_ON(dir != dir_ent->d_inode);
	err = cas_dir_ensure_loaded(dir_ent);
	if (unlikely(err))
		goto out;

	err = cas_dir_remove_rb_node(&dir_node->entries, dentry->d_name.name,
				     dentry->d_name.len, node);

	if (likely(!err)) {
		ts = current_time(&node->inode);
		casfs_inode_set_ctime_to_ts(&node->inode, ts);
		mark_inode_dirty(&node->inode);

		cas_touch_mtime_ctime_ts(dir, ts);
		dir_node->valid_dir_list_hash = false;
	}
	drop_nlink(&node->inode);
	cas_tap_unlink(dentry);

	if (likely(!err))
		casfs_dentry_unpin_on_remove(dentry);
	else
		pr_err("%s: the node %llu not found in tree '%.*s'\n", op,
		       (unsigned long long)node->inode.i_ino,
		       dentry->d_name.len, dentry->d_name.name);

out:
	cas_unlock_node(dir);

	if (likely(!err))
		cas_dir_changed(dir_ent);

	return err;
}

int cas_unlink(struct inode *dir, struct dentry *dentry)
{
	int ret = cas_remove(__func__, dir, dentry);
	return ret;
}

int cas_rmdir(struct inode *dir, struct dentry *dentry)
{
	if (!cas_empty_or_not_loaded(dentry))
		return -ENOTEMPTY;

	return cas_remove(__func__, dir, dentry);
}

static void cas_lock_rename(struct inode *old_dir, struct inode *new_dir)
{
	if (old_dir == new_dir) {
		cas_lock_node(old_dir);
	} else if (old_dir < new_dir) {
		cas_lock_node(old_dir);
		cas_lock_node(new_dir);
	} else {
		cas_lock_node(new_dir);
		cas_lock_node(old_dir);
	}
}

static void cas_unlock_rename(struct inode *old_dir, struct inode *new_dir)
{
	cas_unlock_node(old_dir);

	if (old_dir != new_dir)
		cas_unlock_node(new_dir);
}

int cas_rename(struct mnt_idmap_type *mnt_userns, struct inode *old_dir,
	       struct dentry *old, struct inode *new_dir, struct dentry *new,
	       unsigned int flags)
{
	int err, err2;
	struct timespec64 ts;

	pr_debug("%s %llu\n", __func__, (unsigned long long)old_dir->i_ino);

	if (unlikely(flags)) {
		pr_debug("rename %pd to %pd with flag %d not supported\n", old,
			 new, flags);
		return -EINVAL;
	}

	if (old->d_sb != new->d_sb)
		return -EXDEV;

	if (!cas_empty_or_not_loaded(new))
		return -ENOTEMPTY;

	cas_lock_rename(old_dir, new_dir);
	cas_tap_rename(old, new);

	err = cas_dir_remove_rb_node(&INODE_TO_CAS(old_dir)->entries,
				     old->d_name.name, old->d_name.len,
				     INODE_TO_CAS(d_inode(old)));
	if (unlikely(err)) {
		pr_err("rename-old: the node %llu not found in tree '%.*s'\n",
		       (unsigned long long)d_inode(old)->i_ino, old->d_name.len,
		       old->d_name.name);
		goto out_unlock;
	}

	if (d_really_is_positive(new)) {
		err = cas_dir_remove_rb_node(&INODE_TO_CAS(new_dir)->entries,
					     new->d_name.name, new->d_name.len,
					     INODE_TO_CAS(d_inode(new)));
		if (unlikely(err)) {
			pr_err("rename-new: the node %llu not found in tree '%.*s'\n",
			       (unsigned long long)d_inode(new)->i_ino,
			       new->d_name.len, new->d_name.name);
			goto out_add_old;
		}
	}

	err = cas_dir_add_rb_node(&INODE_TO_CAS(new_dir)->entries,
				  new->d_name.name, new->d_name.len,
				  INODE_TO_CAS(d_inode(old)));
	if (unlikely(err))
		goto out_add_new;

	ts = current_time(old_dir);
	cas_touch_mtime_ctime_ts(old_dir, ts);
	casfs_inode_set_ctime_to_ts(d_inode(old), ts);
	mark_inode_dirty(d_inode(old));

	cas_touch_mtime_ctime_ts(new_dir, ts);

	if (d_really_is_positive(new)) {
		casfs_inode_set_ctime_to_ts(d_inode(new), ts);
		mark_inode_dirty(d_inode(new));
		casfs_dentry_unpin_on_remove(new);
	}

	INODE_TO_CAS(new_dir)->valid_dir_list_hash = false;

	cas_unlock_rename(old_dir, new_dir);

	if (old_dir != new_dir) {
		INODE_TO_CAS(old_dir)->valid_dir_list_hash = false;
		cas_dir_changed(old->d_parent);
	}
	cas_dir_changed(new->d_parent);

	return 0;

out_add_new:
	if (d_really_is_positive(new)) {
		err2 = cas_dir_add_rb_node(&INODE_TO_CAS(new_dir)->entries,
					   new->d_name.name, new->d_name.len,
					   INODE_TO_CAS(d_inode(new)));
		WARN_ON(err2);
	}
out_add_old:
	err2 = cas_dir_add_rb_node(&INODE_TO_CAS(old_dir)->entries,
				   old->d_name.name, old->d_name.len,
				   INODE_TO_CAS(d_inode(old)));
	WARN_ON(err2);

out_unlock:
	cas_unlock_rename(old_dir, new_dir);

	return err;
}

void cas_free_dir(struct cas_node *node)
{
	if (node->is_loaded)
		cas_dir_free_rb_nodes(&node->entries);
}

static int cas_dir_open(struct inode *inode, struct file *file)
{
	int err;

	pr_debug("%s(%llu)\n", __func__, (unsigned long long)inode->i_ino);

	cas_lock_node(inode);
	WARN_ON(inode != file_dentry(file)->d_inode);
	err = cas_dir_ensure_loaded(file_dentry(file));
	cas_unlock_node(inode);

	if (unlikely(err))
		return err;

	return dcache_dir_open(inode, file);
}

int cas_dir_snapshot_list(struct cas_node *node)
{
	SHASH_DESC_ON_STACK(desc, cas_sha1_tfm);
	int err;
	struct cas_rb_node *n;

	if (node->valid_dir_list_hash)
		return 0;

	desc->tfm = cas_sha1_tfm;

	err = crypto_shash_init(desc);
	if (unlikely(err))
		return err;

	for (struct rb_node *rb_node = rb_first(&node->entries); rb_node;
	     rb_node = rb_next(rb_node)) {
		n = rb_entry(rb_node, struct cas_rb_node, rb_node);

		err = crypto_shash_update(desc, n->key, n->name_len);
		if (unlikely(err))
			goto out;

		err = crypto_shash_update(desc, "\0", 1);
		if (unlikely(err))
			goto out;
	}

	err = crypto_shash_final(desc, node->dir_list_hash.bytes);
	if (likely(!err))
		node->valid_dir_list_hash = true;

out:
	shash_desc_zero(desc);
	return err;
}

int cas_get_dir_list_hash(struct cas_node *node, struct cas_hash *hash)
{
	int err;
	struct inode *inode = CAS_TO_INODE(node);

	cas_lock_node(inode);
	err = cas_dir_snapshot_list(node);
	if (likely(!err))
		*hash = node->dir_list_hash;
	cas_unlock_node(inode);
	return err;
}

static int cas_dir_iterate(struct file *file, struct dir_context *ctx)
{
	int err;
	struct inode *inode = file_inode(file);

	pr_debug("%s(%llu, %lld)\n", __func__,
		 (unsigned long long)file->f_inode->i_ino, ctx->pos);

	cas_lock_node(inode);
	cas_tap_readdir(file);
	cas_unlock_node(inode);

	err = dcache_readdir(file, ctx);
	if (unlikely(!err))
		file_accessed(file);
	return err;
}

static loff_t cas_dir_llseek(struct file *file, loff_t offset, int origin)
{
	pr_debug("%s(%llu, %lld, %d)\n", __func__,
		 (unsigned long long)file->f_inode->i_ino, offset, origin);
	return dcache_dir_lseek(file, offset, origin);
}

static int cas_dir_release(struct inode *inode, struct file *file)
{
	pr_debug("%s(%llu)\n", __func__, (unsigned long long)inode->i_ino);
	return dcache_dir_close(inode, file);
}

WRAP_DIR_ITER(cas_dir_iterate)
const struct file_operations cas_dir_operations = {
	.read = generic_read_dir,
	.open = cas_dir_open,
	.iterate_shared = shared_cas_dir_iterate,
	.llseek = cas_dir_llseek,
	.fsync = noop_fsync,
	.release = cas_dir_release,
	.unlocked_ioctl = cas_dir_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = cas_dir_compat_ioctl,
#endif
};

struct dentry *cas_lookup(struct inode *dir, struct dentry *dentry,
			  unsigned int flags)
{
	int err;
	struct dentry *ret;
	struct cas_node *dir_node = INODE_TO_CAS(dir);

	pr_debug("%s %llu '%s' \"%s\" flags=0x%x dent->flags=0x%x\n", __func__,
		 (unsigned long long)dir->i_ino, dentry->d_name.name,
		 dir_node->is_loaded ? "loaded" : "not-loaded", flags,
		 dentry->d_flags);

	cas_lock_node(dir);
	WARN_ON(dir != dentry->d_parent->d_inode);
	err = cas_dir_ensure_loaded(dentry->d_parent);
	if (unlikely(err)) {
		ret = ERR_PTR(err);
		goto out_unlock;
	}

	ret = d_lookup(dentry->d_parent, &dentry->d_name);
	if (IS_ERR(ret))
		goto out_unlock;

	if (!ret) {
		/* Entry not found. */
		dentry->d_inode = NULL;
		d_add(dentry, NULL);
	}

out_unlock:
	cas_unlock_node(dir);
	return ret;
}

const struct inode_operations cas_dir_inode_operations = {
	.lookup = cas_lookup,
	.mkdir = cas_mkdir_op,
	.symlink = cas_symlink,
	.unlink = cas_unlink,
	.rmdir = cas_rmdir,
	.rename = cas_rename,
	.link = cas_link,
	.create = cas_create,
	.mknod = cas_mknod,
	.getattr = cas_getattr,
};

static bool cas_negative_may_be_stale(struct inode *dir)
{
	struct cas_node *node = INODE_TO_CAS(dir);

	if (node->type == CAS_TYPE_CTRL)
		return false;

	if (node->type == CAS_TYPE_GIT_OBJECT_DIR)
		return true;

	return !READ_ONCE(node->is_loaded);
}

int casfs_revalidate(struct dentry *dentry, unsigned int flags)
{
	struct dentry *parent;
	struct inode *dir;
	int ret = 1;

	/* Positive entries are backed by CAS and are stable. */
	if (d_really_is_positive(dentry))
		return 1;

	if (flags & LOOKUP_RCU) {
		parent = READ_ONCE(dentry->d_parent);
		dir = d_inode_rcu(parent);
		if (!dir)
			return -ECHILD;
		if (cas_negative_may_be_stale(dir))
			return -ECHILD; /* re-check with a stable parent */
		return 1;
	}

	parent = dget_parent(dentry);
	dir = d_inode(parent);

	if (dir && cas_negative_may_be_stale(dir))
		ret = 0;

	dput(parent);
	return ret;
}

const struct dentry_operations casfs_dentry_operations = {
	.d_revalidate = casfs_revalidate_op,
};
