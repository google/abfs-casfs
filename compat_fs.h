/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Google LLC
 */

#ifndef _CASFS_COMPAT_FS_H_
#define _CASFS_COMPAT_FS_H_

#include <linux/fs.h>
#include <linux/version.h>
#include <linux/fs_parser.h>

static inline void casfs_kill_super(struct super_block *sb)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
	kill_anon_super(sb);
#else
	kill_litter_super(sb);
#endif
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
#define CAS_FSPARAM_PATH(name, opt) fsparam_string(name, opt)
#else
#define CAS_FSPARAM_PATH(name, opt) fsparam_path(name, opt)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 0, 0)
static inline int cas_remount_fs(struct super_block *sb, int *flags, char *data)
{
	return 0;
}

#define CAS_SUPER_OPS_REMOUNT() .remount_fs = cas_remount_fs,
#else
#define CAS_SUPER_OPS_REMOUNT()
#endif

#endif /* _CASFS_COMPAT_FS_H_ */
