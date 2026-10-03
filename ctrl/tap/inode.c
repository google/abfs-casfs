// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 */

#include "inode.h"
#include "ctrl.h"
#include "tap.h"
#include "session.h"

static size_t tap_data_calc_exact_size(struct tap_data *data)
{
	size_t len = 0;

	if (!data)
		return 0;

	// Fixed fields: type(1) + dirty(1) + is_ari(1) + size(8) + mode(4)
	// + null terminators for paths(2)
	len += 1 + 1 + 1 + sizeof(data->size) + sizeof((int32_t)data->mode) + 2;

	if (data->is_ari)
		len += (data->ari ? strlen(data->ari) : 0) + 1; // +1 for '\0'
	else
		len += CASFS_HASH_LEN;

	if (data->path.start)
		len += strlen(data->path.start);

	if (data->aux_path.start)
		len += strlen(data->aux_path.start);

	return len;
}

/* Serializes a single tap_data entry into a contiguous memory buffer.
 * Returns bytes written, or -ENOSPC if it doesn't fit in remaining space.
 */
static int tap_data_serialize(struct tap_data *data, char *buf, size_t max_len)
{
	size_t len = 0;
	size_t req = tap_data_calc_exact_size(data);
	char *p = buf;
	int32_t standard_mode;

	if (req > max_len)
		return -ENOSPC;

	*p++ = (char)data->type;
	*p++ = (char)data->dirty;
	*p++ = (char)data->is_ari;

	if (data->is_ari) {
		if (data->ari) {
			len = strlen(data->ari);
			memcpy(p, data->ari, len);
			p += len;
		}
		*p++ = '\0';
	} else {
		memcpy(p, data->hash.bytes, CASFS_HASH_LEN);
		p += CASFS_HASH_LEN;
	}

	memcpy(p, &data->size, sizeof(data->size));
	p += sizeof(data->size);

	standard_mode = (int32_t)data->mode;
	memcpy(p, &standard_mode, sizeof(standard_mode));
	p += sizeof(standard_mode);

	if (data->path.start) {
		len = strlen(data->path.start);
		memcpy(p, data->path.start, len);
		p += len;
	}
	*p++ = '\0';

	if (data->aux_path.start) {
		len = strlen(data->aux_path.start);
		memcpy(p, data->aux_path.start, len);
		p += len;
	}
	*p++ = '\0';

	return p - buf;
}

static inline bool tap_session_has_events(struct session *s)
{
	/* Pairs with smp_store_release() in session.c */
	return !list_empty_careful(&s->head) || smp_load_acquire(&s->done) ||
	       READ_ONCE(s->is_overflow);
}
static ssize_t tapfs_read(struct file *filp, char __user *user_buf,
			  size_t count, loff_t *ppos)
{
	struct session *s = (struct session *)filp->private_data;
	char *kbuf;
	size_t batch_size;
	size_t written = 0;
	LIST_HEAD(free_list);
	ssize_t ret = 0;
	bool sending_version = false;

	if (!s)
		return -EINVAL;

	batch_size = min_t(size_t, count, 64 * 1024);
	kbuf = kvmalloc(batch_size, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	for (;;) {
		mutex_lock(&s->lock);

		if (s->is_overflow) {
			mutex_unlock(&s->lock);
			ret = -ENOSPC;
			goto out_free;
		}

		if (!list_empty(&s->head))
			break;

		/* Mutex acquisition provides a full barrier */
		if (s->done) {
			mutex_unlock(&s->lock);
			ret = 0; /* EOF */
			goto out_free;
		}

		mutex_unlock(&s->lock);

		if (filp->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out_free;
		}

		if (wait_event_interruptible(s->wait,
					     tap_session_has_events(s))) {
			ret = -ERESTARTSYS;
			goto out_free;
		}
	}

	/* s->lock is held from the break above */

	if (!s->version_sent) {
		if (batch_size < 1) {
			mutex_unlock(&s->lock);
			ret = -EMSGSIZE;
			goto out_free;
		}
		kbuf[written++] = TAP_SEQ_VERSION;
		sending_version = true;
	}

	/*
	 * Serialize as many records as can fit into kbuf.
	 * Move successfully serialized items to free_list.
	 */
	while (!list_empty(&s->head)) {
		struct tap_data *data =
			list_first_entry(&s->head, struct tap_data, list);
		int item_len;

		item_len = tap_data_serialize(data, kbuf + written,
					      batch_size - written);
		if (item_len < 0) {
			if (list_empty(&free_list))
				ret = -EMSGSIZE;
			break;
		}

		written += item_len;
		list_move_tail(&data->list, &free_list);
		--s->entries_cur;
	}

	mutex_unlock(&s->lock);

	/*
	 * Free drained events immediately outside s->lock. If copy_to_user
	 * fails (-EFAULT), we deliberately do not restore these items to
	 * s->head: an EFAULT indicates a fatal userspace reader crash, in
	 * which case the build tapping graph is already tainted and must fail.
	 */
	while (!list_empty(&free_list)) {
		struct tap_data *data =
			list_first_entry(&free_list, struct tap_data, list);
		list_del(&data->list);
		tap_data_free(data);
	}

	/* Copy the serialized buffer to user-space */
	if (written > 0 && ret >= 0) {
		if (copy_to_user(user_buf, kbuf, written))
			ret = -EFAULT;
		else
			ret = written;
	}

	if (sending_version && ret > 0) {
		mutex_lock(&s->lock);
		s->version_sent = true;
		mutex_unlock(&s->lock);
	}

out_free:
	kvfree(kbuf);
	return ret;
}

/* Creating temporary file starts tapping*/
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
static int tapfs_tmpfile(struct mnt_idmap_type *mnt_userns, struct inode *dir,
			 struct dentry *dentry, umode_t mode)
{
	int rc = 0;
	struct inode *inode =
		cas_ctrl_iget(dir->i_sb, dir, mode, CAS_CTRL_TYPE_TAP_FILE);

	if (!inode)
		return -ENOSPC;

	d_tmpfile(dentry, inode);
	return rc;
}
static int tapfs_open(struct inode *inode, struct file *filp)
{
	struct session *session = NULL;
	struct tap_ctrl *tap_ctrl = SB_TO_CAS_CTRL(inode->i_sb)->tap;
	int rc = 0;

	session = tap_session_user_open(tap_ctrl);
	if (session == NULL)
		return -EINVAL;

	rc = simple_open(inode, filp);
	if (rc != 0)
		goto outerr;

	filp->private_data = session;
	return rc;

outerr:
	mutex_lock(&tap_ctrl->lock);
	hash_del(&session->node);
	mutex_unlock(&tap_ctrl->lock);
	tap_session_put(session);
	return rc;
}
#else
static int tapfs_tmpfile(struct mnt_idmap_type *mnt_userns, struct inode *dir,
			 struct file *filp, umode_t mode)
{
	int rc = 0;
	struct tap_ctrl *tap_ctrl = SB_TO_CAS_CTRL(dir->i_sb)->tap;
	struct inode *inode =
		cas_ctrl_iget(dir->i_sb, dir, mode, CAS_CTRL_TYPE_TAP_FILE);
	struct session *session = NULL;

	if (!inode)
		return -ENOSPC;

	session = tap_session_user_open(tap_ctrl);
	if (!session)
		return -EINVAL;

	d_tmpfile(filp, inode);
	rc = finish_open_simple(filp, 0);

	if (rc != 0)
		goto outerr;

	filp->private_data = session;

	return rc;

outerr:
	mutex_lock(&tap_ctrl->lock);
	hash_del(&session->node);
	mutex_unlock(&tap_ctrl->lock);
	tap_session_put(session);

	return rc;
}
#endif //#if LINUX_VERSION_CODE < KERNEL_VERSION(6,1,0)

static __poll_t tapfs_poll(struct file *filp, poll_table *pt)
{
	struct session *session = (struct session *)filp->private_data;

	if (session == NULL)
		return 0;

	return tap_session_poll(session, filp, pt);
}

static long tapfs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct session *s = (struct session *)filp->private_data;

	if (!s)
		return -EINVAL;

	switch (cmd) {
	case CASFS_IOC_TAP_FINISHED:
	case CASFS_IOC_TAP_FINISHED_OLD:
		tap_session_set_done(s);
		return 0;
	default:
		return -ENOTTY;
	}
}

static int tapfs_release(struct inode *inode, struct file *file)
{
	struct session *s = file->private_data;

	if (s) {
		tap_session_close_and_flush(s);

		mutex_lock(&s->ctrl->lock);
		hash_del(&s->node);
		mutex_unlock(&s->ctrl->lock);

		tap_session_put(s);
		file->private_data = NULL;
	}
	return 0;
}

const struct inode_operations tapfs_dir_inode_operations = {
	.lookup = simple_lookup,
	.tmpfile = tapfs_tmpfile,
	.rmdir = simple_rmdir,
	.unlink = simple_unlink,
};

const struct file_operations tapfs_file_operations = {
	.read = tapfs_read,
	.release = tapfs_release,
	.poll = tapfs_poll,
	.unlocked_ioctl = tapfs_ioctl,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
	.open = tapfs_open,
#endif
};

const struct inode_operations tapfs_file_inode_operations = {
	.setattr = simple_setattr,
	.getattr = simple_getattr,
};
