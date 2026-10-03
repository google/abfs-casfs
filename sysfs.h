/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Google LLC
 */
#ifndef _CASFS_SYSFS_H_
#define _CASFS_SYSFS_H_

#include <linux/kobject.h>
#include <linux/completion.h>

int casfs_sysfs_module_init(void);
void casfs_sysfs_module_exit(void);

struct cas_fs;
struct casfs_sysfs {
	struct kobject kobj;
	struct completion kobj_unregister;
	struct cas_fs *fs;
};

int casfs_sysfs_register(struct cas_fs *fs, struct casfs_sysfs *sysfs);
void casfs_sysfs_unregister(struct casfs_sysfs *sysfs);

void casfs_sysfs_notify_upstream_status(struct casfs_sysfs *sysfs);

#endif /* _CASFS_SYSFS_H_ */
