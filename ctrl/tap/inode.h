/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google LLC
 */
#ifndef _CTRL_TAP_INODE_H
#define _CTRL_TAP_INODE_H

#include "../inode.h"

extern const struct inode_operations tapfs_dir_inode_operations;
extern const struct file_operations tapfs_file_operations;
extern const struct inode_operations tapfs_file_inode_operations;

#endif /* _CTRL_TAP_INODE_H */
