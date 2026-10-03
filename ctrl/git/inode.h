/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google LLC
 */
#ifndef _CTRL_GIT_INODE_H
#define _CTRL_GIT_INODE_H

#include "../inode.h"

extern const struct inode_operations gitfs_objects_dir_inode_operations;
extern const struct inode_operations gitfs_object_dir_inode_operations;
extern const struct inode_operations gitfs_object_inode_operations;
extern const struct file_operations gitfs_object_file_operations;

extern int create_git_object_dir_locked(struct dentry *dir,
					const char *dir_name,
					size_t dir_name_len);

#endif /* _CTRL_GIT_INODE_H */
