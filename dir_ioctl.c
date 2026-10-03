// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Google LLC
 */
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/compat.h>
#include <linux/limits.h>
#include <linux/string.h>

#include "casfs.h"
#include "node.h"
#include "dir_rb_tree.h"
#include "compat_inode.h"

/* Max record size: header + NAME_MAX + 1 + CAS_MAX_ARI_LEN + 1 */
#define CASFS_MAX_RECORD_SIZE \
	(sizeof(struct casfs_dir_entry) + NAME_MAX + 1 + 4096 + 1)

long cas_dir_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(file);
	struct cas_node *dir_node = INODE_TO_CAS(inode);
	struct casfs_read_dir_args args;
	void __user *uarg = (void __user *)arg;
	struct rb_node *rb_node;
	char *bounce_buf;
	struct casfs_dir_entry *entry;
	size_t written = 0;
	u32 count = 0;
	u64 current_idx = 0;
	int ret = 0;

	if (cmd != CASFS_IOC_READ_DIR_ENTRIES)
		return -ENOTTY;

	ret = casfs_file_permission(file, inode, MAY_READ);
	if (ret)
		return ret;

	if (copy_from_user(&args, uarg, sizeof(args)))
		return -EFAULT;

	if (!args.buf_ptr ||
	    args.buf_size < (sizeof(struct casfs_dir_entry) + 2) ||
	    args.buf_size > (32 * 1024 * 1024) ||
	    (args.buf_ptr + args.buf_size) < args.buf_ptr)
		return -EINVAL;

	if (!access_ok((void __user *)(uintptr_t)args.buf_ptr, args.buf_size))
		return -EFAULT;

	bounce_buf = kmalloc(CASFS_MAX_RECORD_SIZE, GFP_KERNEL);
	if (!bounce_buf)
		return -ENOMEM;

	entry = (struct casfs_dir_entry *)bounce_buf;

	cas_lock_node(inode);

	if (!dir_node->is_loaded) {
		ret = cas_dir_ensure_loaded(file->f_path.dentry);
		if (ret)
			goto out_unlock;
	}

	args.has_more = 0;

	for (rb_node = rb_first(&dir_node->entries); rb_node;
	     rb_node = rb_next(rb_node)) {
		struct cas_rb_node *n =
			rb_entry(rb_node, struct cas_rb_node, rb_node);
		const struct cas_node *child = n->node;
		struct cas_hash h, commit_h;
		u64 file_size = 0;
		size_t rec_len;
		size_t name_len = n->name_len;
		size_t ari_len = 0;
		const char *ari_str = NULL;
		struct timespec64 ctime;
		struct timespec64 mtime;
		u16 flags = 0;
		unsigned int seq;

		/* Resume cookie check */
		if (current_idx < args.cookie) {
			current_idx++;
			continue;
		}

		if (name_len > NAME_MAX) {
			ret = -EINVAL;
			goto out_unlock;
		}

		/* Extract ARI if it's a regular file */
		if (child->type == CAS_TYPE_REG ||
		    child->type == CAS_TYPE_REG_EXEC) {
			if (child->ari) {
				ari_str = child->ari;
				ari_len = strnlen(ari_str, CAS_MAX_ARI_LEN + 1);
				if (ari_len > CAS_MAX_ARI_LEN) {
					ret = -EINVAL;
					goto out_unlock;
				}
				flags |= CASFS_FLAG_HAS_ARI;
			}
		}

		rec_len = sizeof(*entry) + (name_len + 1) + (ari_len + 1);

		/* Check if entry fits in remaining buffer */
		if (written + rec_len > args.buf_size) {
			args.has_more = 1;
			break;
		}

		memset(entry, 0, sizeof(*entry));

		ctime = casfs_inode_get_ctime(&child->inode);
		mtime = casfs_inode_get_mtime(&child->inode);

		entry->ctime_sec = (u32)ctime.tv_sec;
		entry->ctime_nsec = (u32)ctime.tv_nsec;
		entry->mtime_sec = (u32)mtime.tv_sec;
		entry->mtime_nsec = (u32)mtime.tv_nsec;
		entry->size = i_size_read(&child->inode);
		entry->ino = child->inode.i_ino;
		entry->dev = child->inode.i_sb->s_dev;
		entry->mode = child->inode.i_mode;
		entry->uid = from_kuid(&init_user_ns, child->inode.i_uid);
		entry->gid = from_kgid(&init_user_ns, child->inode.i_gid);
		entry->type = child->type;
		entry->backend_type = child->backend_type;
		entry->dirtiness = atomic_read(&child->dirtiness);

		if (child->is_loaded)
			flags |= CASFS_FLAG_LOADED;
		if (child->is_excluded)
			flags |= CASFS_FLAG_EXCLUDED;
		if (child->debug)
			flags |= CASFS_FLAG_DEBUG;

		entry->flags = flags;
		entry->name_len = (__u16)name_len;
		entry->ari_len = (__u16)ari_len;

		if (child->type != CAS_TYPE_CTRL) {
			do {
				seq = read_seqbegin(&child->hash_lock);
				h = child->hash;
				commit_h = child->commit_hash;
				file_size = i_size_read(&child->inode);
			} while (read_seqretry(&child->hash_lock, seq));
		} else {
			memset(&h, 0, sizeof(h));
			memset(&commit_h, 0, sizeof(commit_h));
			file_size = i_size_read(&child->inode);
		}

		entry->size = file_size;
		memcpy(entry->sha, h.bytes, CASFS_HASH_LEN);
		memcpy(entry->commit_sha, commit_h.bytes, CASFS_HASH_LEN);

		/* Copy name + null byte */
		memcpy(entry->payload, n->key, name_len);
		entry->payload[name_len] = '\0';

		/* Copy ARI + null byte */
		if (ari_len > 0)
			memcpy(&entry->payload[name_len + 1], ari_str, ari_len);

		entry->payload[name_len + 1 + ari_len] = '\0';

		if (copy_to_user(
			    (void __user *)(uintptr_t)(args.buf_ptr + written),
			    bounce_buf, rec_len)) {
			ret = -EFAULT;
			goto out_unlock;
		}

		written += rec_len;
		count++;
		current_idx++;
	}

	args.bytes_written = written;
	args.entries_count = count;
	args.cookie = current_idx;

	if (copy_to_user(uarg, &args, sizeof(args)))
		ret = -EFAULT;

out_unlock:
	cas_unlock_node(inode);
	kfree(bounce_buf);
	return ret;
}

#ifdef CONFIG_COMPAT
long cas_dir_compat_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	return cas_dir_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif
