/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google LLC
 */

#ifndef _TAP_TAP_H_
#define _TAP_TAP_H_

#include "../../node.h"

#define TAP_SEQ_VERSION 3

enum tap_trigger {
	TAP_MKDIR = 0,
	TAP_SYMLINK = 1,
	TAP_LINK = 2,
	TAP_UNLINK = 3,
	TAP_RENAME = 4,
	TAP_READDIR = 5,
	TAP_SYMLINK_READ = 6,
	TAP_STAT = 7,
	TAP_OPEN_READ = 8,
	TAP_OPEN_WRITE = 9,
	TAP_ATTR_CHANGE = 10,
};

struct tap_ctrl;

struct tap_data_path {
	char *start;
	char *buf;
};

struct tap_data {
	struct list_head list;

	struct tap_ctrl *ctrl;
	loff_t size;

	struct tap_data_path path;
	struct tap_data_path aux_path;

	union {
		struct cas_hash hash;
		char *ari;
	};
	enum tap_trigger type;
	umode_t mode;
	bool dirty;
	bool is_ari;
};

extern uint32_t tap_session_id(void);
extern const char *tap_name(enum tap_trigger type);
extern void tap_data_free(struct tap_data *data);

#endif /*_TAP_TAP_H_*/
