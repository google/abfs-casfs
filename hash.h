/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google LLC
 */

#ifndef _CASFS_HASH_H_
#define _CASFS_HASH_H_

#include "common.h"

#define CASFS_HASH_LEN 20
#define CASFS_HASH_HEX_LEN (2 * CASFS_HASH_LEN)

struct cas_hash {
	u8 bytes[CASFS_HASH_LEN];
};

int cas_hash_init(void);
void cas_hash_exit(void);
extern struct crypto_shash *cas_sha1_tfm;
extern const char *cas_zero_hash_hex;
extern const struct cas_hash cas_zero_hash;

extern const char *cas_empty_file_hash_hex;
extern const struct cas_hash cas_empty_file_hash;

extern const char *cas_empty_dir_hash_hex;
extern const struct cas_hash cas_empty_dir_hash;

extern const u8 cas_empty_dir_content[];
extern const int cas_empty_dir_content_len;

static inline int cas_hash_cmp(const struct cas_hash *h1,
			       const struct cas_hash *h2)
{
	return memcmp(h1->bytes, h2->bytes, CASFS_HASH_LEN);
}

extern void cas_hash_from_bytes(u8 *bytes, struct cas_hash *h);
extern int cas_hash_from_hex(const char *hex, int len, struct cas_hash *h);

extern char *cas_hash_to_hex(const struct cas_hash *h);
extern void cas_hash_fill_hex(const struct cas_hash *h, char *buf);

extern int cas_hash_bytes(const void *buf, ssize_t len, struct cas_hash *h);
extern int cas_hash_blob(const void *buf, ssize_t len, struct cas_hash *h);
extern int cas_hash_file(struct file *file, struct cas_hash *h, loff_t *size,
			 bool is_blob);

#endif // _CASFS_HASH_H_
