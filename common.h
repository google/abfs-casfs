/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google LLC
 */

#ifndef _CASFS_COMMON_H_
#define _CASFS_COMMON_H_

#ifndef KBUILD_MODNAME
#define KBUILD_MODNAME "casfs"
#endif

#define byte_t unsigned char
#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#endif /*_CASFS_COMMON_H_*/
