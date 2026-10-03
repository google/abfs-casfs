// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 */
#include "session.h"
#include "../inode.h"

inline uint32_t tap_session_id(void)
{
	return pid_nr(task_pgrp(current));
}

static void tap_session_record_stats(struct session *s)
{
	int bucket;

	if (!s->ctrl)
		return;

	mutex_lock(&s->ctrl->lock);

	// 1. Log overall peak ceiling tracker
	if ((u64)s->entries_hwm > s->ctrl->peak_per_session_hwm)
		s->ctrl->peak_per_session_hwm = s->entries_hwm;

	// 2. Classify the session queue depth into a power-of-two bucket
	if (s->entries_hwm <= 64)
		bucket = 0;
	else if (s->entries_hwm <= 256)
		bucket = 1;
	else if (s->entries_hwm <= 1024)
		bucket = 2;
	else if (s->entries_hwm <= 4096)
		bucket = 3;
	else if (s->entries_hwm <= 16384)
		bucket = 4;
	else if (s->entries_hwm <= 65536)
		bucket = 5;
	else if (s->entries_hwm <= 262144)
		bucket = 6;
	else if (s->entries_hwm <= 1048576)
		bucket = 7;
	else
		bucket = 8;

	s->ctrl->peak_depth_histogram[bucket]++;
	mutex_unlock(&s->ctrl->lock);
}

void tap_session_set_done(struct session *s)
{
	if (!s)
		return;

	mutex_lock(&s->lock);
	/*
	 * Pairs with smp_load_acquire() in tapfs_read().
	 * Ensures all prior memory writes to queue data/stats are committed
	 * to the cache coherency fabric before the 'done' state flag is made
	 * visible to reader CPU cores.
	 */
	smp_store_release(&s->done, true);
	wake_up_interruptible(&s->wait);
	mutex_unlock(&s->lock);
}

bool tap_session_is_done(struct session *s)
{
	bool out = false;

	if (!s)
		return false;

	mutex_lock(&s->lock);
	out = s->done;
	mutex_unlock(&s->lock);

	return out;
}

void tap_session_add(struct session *s, struct tap_data *data)
{
	mutex_lock(&s->lock);

	if (s->is_overflow) {
		mutex_unlock(&s->lock);
		tap_data_free(data);
		return;
	}
	list_add_tail(&data->list, &s->head);
	wake_up_interruptible(&s->wait);
	++s->entries;
	++s->entries_cur;

	if (s->entries_cur > s->entries_hwm)
		s->entries_hwm = s->entries_cur;
	mutex_unlock(&s->lock);
}

/* Destroy tap session, delete tap data */
static void tap_session_destroy(struct session *s)
{
	bool unread = false;
	struct tap_data *data, *tmp;

	tap_session_record_stats(s);

	list_for_each_entry_safe(data, tmp, &s->head, list) {
		list_del(&data->list);
		tap_data_free(data);
		unread = true;
	}

	kfree(s);

	if (unread)
		pr_err("destroying session with data not read out by client\n");
}

static void tap_session_destroy_kref(struct kref *ref)
{
	struct session *session = container_of(ref, struct session, refcount);

	tap_session_destroy(session);
}

void tap_session_put(struct session *session)
{
	if (session)
		kref_put(&session->refcount, tap_session_destroy_kref);
}

struct session *tap_session_user_open(struct tap_ctrl *ctrl)
{
	uint32_t sid = tap_session_id();
	struct session *out = kzalloc(sizeof(struct session), GFP_KERNEL);

	if (unlikely(!out))
		return NULL;

	kref_init(&out->refcount);
	INIT_LIST_HEAD(&out->head);
	mutex_init(&out->lock);
	init_waitqueue_head(&out->wait);

	out->sid = sid;
	out->ctrl = ctrl;

	mutex_lock(&ctrl->lock);
	hash_add(ctrl->sessions, &out->node, sid);
	mutex_unlock(&ctrl->lock);

	return out;
}

void tap_session_close_and_flush(struct session *s)
{
	if (!s)
		return;

	mutex_lock(&s->lock);
	/*
	 * Pairs with smp_load_acquire() in tapfs_read().
	 * Ensures that the queue's termination state is cleanly visible
	 * to active reader threads waking up on other CPU cores during
	 * abrupt VFS file descriptor teardown.
	 */
	smp_store_release(&s->done, true);
	mutex_unlock(&s->lock);

	/* Unblock any reads executing on concurrent cores */
	wake_up_all(&s->wait);
}
int tap_session_entries(struct session *s)
{
	int out = 0;

	mutex_lock(&s->lock);
	out = s->entries_cur;
	mutex_unlock(&s->lock);

	return out;
}

__poll_t tap_session_poll(struct session *s, struct file *filp, poll_table *pt)
{
	__poll_t mask = 0;

	poll_wait(filp, &s->wait, pt);

	mutex_lock(&s->lock);

	if (!list_empty(&s->head))
		mask |= EPOLLIN | EPOLLRDNORM;

	if (s->done)
		mask |= EPOLLIN | EPOLLRDNORM;

	mutex_unlock(&s->lock);

	return mask;
}
