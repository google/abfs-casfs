/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google LLC
 */

#ifndef _CTRL_UTIL_H
#define _CTRL_UTIL_H
#include <linux/fs.h>

/* Perform kernel_write return 0 if all data written error otherwise */
extern int ctrl_kernel_write(struct file *file, const void *buf, size_t count,
			     loff_t *pos);

#endif /* _CTRL_UTIL_H */
