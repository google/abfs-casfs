/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google LLC
 */
#ifndef _CTRL_INODE_H
#define _CTRL_INODE_H
#include <linux/fs.h>

#include "../node.h"
#include "../fs.h"

extern int ctrl_mkdir(struct inode *dir, struct dentry *dentry,
		      enum cas_ctrl_type type);
extern int ctrl_create(struct inode *dir, struct dentry *dentry,
		       enum cas_ctrl_type type);
extern int ctrl_create_with_stat(struct inode *dir, struct dentry *dentry,
				 enum cas_ctrl_type type,
				 struct cas_stat_info *stat);

extern struct inode *cas_ctrl_iget(struct super_block *sb,
				   const struct inode *dir, umode_t mode,
				   enum cas_ctrl_type type);
static inline struct cas_ctrl *SB_TO_CAS_CTRL(struct super_block *sb)
{
	return SB_TO_FS(sb)->ctrl;
}
static inline struct cas_ctrl *FILE_TO_CAS_CTRL(struct file *file)
{
	return SB_TO_FS(file_inode(file)->i_sb)->ctrl;
}
#endif /* _CTRL_INODE_H */
