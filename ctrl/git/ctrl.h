/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google LLC
 */
#ifndef _INC_GIT_CTRL_H
#define _INC_GIT_CTRL_H

#include <linux/fs.h>
#include "../../provider.h"

#define PEEK_BUF_SIZE 256 // Must be less than PAGE_SIZE

extern struct git_ctrl *git_init(void);
extern void git_destroy(struct git_ctrl *ctrl);

/* Objects */
extern loff_t cas_to_git_object_write(struct git_ctrl *ctrl, bool is_blob,
				      struct file *cas_file,
				      struct file *git_file);

extern int git_to_cas_object_write(struct git_ctrl *ctrl,
				   struct cas_provider *provider,
				   struct file *git_file,
				   const struct cas_hash *hash);

/* Uncompress a small amount just to read the header */
extern int peek_git_type(struct git_ctrl *ctrl, struct file *git_file,
			 enum cas_req_obj_type *obj_type);

#endif /* _INC_GIT_CTRL_H */
