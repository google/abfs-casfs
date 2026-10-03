/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 Google LLC
 */
#ifndef _INC_TAP_SESSION_H
#define _INC_TAP_SESSION_H
#include <linux/dcache.h>
#include <linux/hashtable.h>

#include "tap.h"

struct session {
	struct mutex lock;
	struct list_head head;
	struct hlist_node node;
	struct kref refcount;
	wait_queue_head_t wait;
	uint32_t sid;
	struct tap_ctrl *ctrl;
	u64 entries;
	int entries_cur;
	int entries_hwm;
	bool done;
	bool is_overflow;
	bool version_sent;
};
extern struct session *tap_session_user_open(struct tap_ctrl *ctrl);

extern int tap_session_entries(struct session *session);

extern __poll_t tap_session_poll(struct session *session, struct file *filp,
				 poll_table *pt);

/*
 * Get session for given session id, increments reference count
 */
extern struct session *tap_session_get(struct tap_ctrl *ctrl, uint32_t sid);
extern void tap_session_put(struct session *s);

extern void tap_session_close_and_flush(struct session *s);

/* Add tap data to a given session */
extern void tap_session_add(struct session *s, struct tap_data *d);

void tap_session_set_done(struct session *s);
bool tap_session_is_done(struct session *s);
#endif /* _INC_TAP_SESSION_H */
