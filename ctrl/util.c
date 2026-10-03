// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 */

#include "../compat.h"
#include "../cas_ctrl.h"
#include "util.h"
#include "inode.h"

int ctrl_kernel_write(struct file *file, const void *buf, size_t count,
		      loff_t *pos)
{
	int rc = 0;

	if (count == 0)
		return rc;

	rc = kernel_write(file, buf, count, pos);

	if (rc < 0)
		return rc;

	return (rc == count) ? 0 : -EINVAL;
}
