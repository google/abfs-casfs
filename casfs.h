/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google LLC
 */

#ifndef _CASFS_H_
#define _CASFS_H_

#include <linux/fs.h>
#include "node.h"
#include "common.h"

#define CASFS_MAGIC 0xca5f5000
// 200GB to approximately hold an archive of an out/ directory
#define CASFS_MAX_FILESIZE (1024ULL * 1024 * 1024 * 200)

static inline void path_put_init_(struct path *path)
{
	path_put(path);
	*path = (struct path){};
}

#endif /*_CASFS_H_*/
