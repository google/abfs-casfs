// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/crypto.h>
#include <crypto/hash.h>
#include "hash.h"
#include "casfs.h"

#define MAX_HEADER_SIZE 32

struct crypto_shash *cas_sha1_tfm;

const char *cas_zero_hash_hex = "0000000000000000000000000000000000000000";
const struct cas_hash cas_zero_hash = {
	.bytes = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, },
};

const char *cas_empty_file_hash_hex =
	"e69de29bb2d1d6434b8b29ae775ad8c2e48c5391";
const struct cas_hash cas_empty_file_hash = {
	.bytes = {0xe6, 0x9d, 0xe2, 0x9b, 0xb2, 0xd1, 0xd6, 0x43, 0x4b, 0x8b,
		  0x29, 0xae, 0x77, 0x5a, 0xd8, 0xc2, 0xe4, 0x8c, 0x53, 0x91, },
};

const char *cas_empty_dir_hash_hex = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";
const struct cas_hash cas_empty_dir_hash = {
	.bytes = {0x4b, 0x82, 0x5d, 0xc6, 0x42, 0xcb, 0x6e, 0xb9, 0xa0, 0x60,
		  0xe5, 0x4b, 0xf8, 0xd6, 0x92, 0x88, 0xfb, 0xee, 0x49, 0x04, },
};

const u8 cas_empty_dir_content[] = { 0x74, 0x72, 0x65, 0x65, 0x20, 0x30, 0x00 };
const int cas_empty_dir_content_len = sizeof(cas_empty_dir_content);

int cas_hash_init(void)
{
	cas_sha1_tfm = crypto_alloc_shash("sha1", 0, 0);
	if (IS_ERR(cas_sha1_tfm)) {
		pr_err("casfs: failed to allocate sha1 tfm\n");
		return PTR_ERR(cas_sha1_tfm);
	}
	return 0;
}

void cas_hash_exit(void)
{
	if (cas_sha1_tfm) {
		crypto_free_shash(cas_sha1_tfm);
		cas_sha1_tfm = NULL;
	}
}

static inline u8 from_hex(u8 c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return 0xff;
}

static inline u8 to_hex(u8 c)
{
	return c < 10 ? '0' + c : 'a' - 10 + c;
}

void cas_hash_from_bytes(u8 *bytes, struct cas_hash *h)
{
	memcpy(h->bytes, bytes, CASFS_HASH_LEN);
}

int cas_hash_from_hex(const char *hex, int len, struct cas_hash *h)
{
	u8 d, c;

	if (unlikely(len != CASFS_HASH_HEX_LEN))
		return -EINVAL;

	for (int i = 0; i < CASFS_HASH_LEN; i++, hex += 2) {
		c = from_hex(*hex);
		if (c == 0xff)
			return -EINVAL;
		d = from_hex(*(hex + 1));
		if (d == 0xff)
			return -EINVAL;
		h->bytes[i] = (c << 4) + d;
	}
	return 0;
}

void cas_hash_fill_hex(const struct cas_hash *h, char *hex)
{
	const u8 *hash_bytes = h->bytes;

	for (int i = 0; i < CASFS_HASH_LEN; i++) {
		hex[2 * i] = to_hex(hash_bytes[i] / 16);
		hex[2 * i + 1] = to_hex(hash_bytes[i] % 16);
	}
	hex[CASFS_HASH_HEX_LEN] = 0;
}

char *cas_hash_to_hex(const struct cas_hash *h)
{
	char *buf = kmalloc(CASFS_HASH_HEX_LEN + 1, GFP_KERNEL);

	if (unlikely(!buf))
		return NULL;

	cas_hash_fill_hex(h, buf);

	return buf;
}

int cas_hash_bytes(const void *buf, ssize_t len, struct cas_hash *h)
{
	SHASH_DESC_ON_STACK(desc, cas_sha1_tfm);
	int err;

	desc->tfm = cas_sha1_tfm;

	err = crypto_shash_digest(desc, buf, len, h->bytes);
	shash_desc_zero(desc);

	return err;
}

int cas_hash_blob(const void *buf, ssize_t len, struct cas_hash *h)
{
	SHASH_DESC_ON_STACK(desc, cas_sha1_tfm);
	char header[MAX_HEADER_SIZE];
	int hn, err;

	desc->tfm = cas_sha1_tfm;

	err = crypto_shash_init(desc);
	if (err)
		return err;

	hn = snprintf(header, MAX_HEADER_SIZE, "blob %zd", len);
	err = crypto_shash_update(desc, header, hn + 1);
	if (err)
		goto out;

	err = crypto_shash_update(desc, buf, len);
	if (err)
		goto out;

	err = crypto_shash_final(desc, h->bytes);

out:
	shash_desc_zero(desc);
	return err;
}

int cas_hash_file(struct file *file, struct cas_hash *h, loff_t *size,
		  bool is_blob)
{
	SHASH_DESC_ON_STACK(desc, cas_sha1_tfm);
	char header[MAX_HEADER_SIZE];
	char *buf;
	int hn, err;
	loff_t len = i_size_read(file_inode(file)), offset, n;

	offset = vfs_llseek(file, 0, SEEK_SET);
	if (unlikely(offset < 0))
		return offset;

	buf = (char *)__get_free_page(GFP_KERNEL);
	if (unlikely(!buf))
		return -ENOMEM;

	desc->tfm = cas_sha1_tfm;

	err = crypto_shash_init(desc);
	if (unlikely(err))
		goto free_page;

	if (is_blob) {
		hn = snprintf(header, MAX_HEADER_SIZE, "blob %lld", len);
		err = crypto_shash_update(desc, header, hn + 1);
		if (err)
			goto out;
	}

	for (offset = 0; offset < len;) {
		n = len - offset > PAGE_SIZE ? PAGE_SIZE : len - offset;
		n = kernel_read(file, buf, n, &offset);
		if (n <= 0) {
			err = n < 0 ? n : -EIO;
			goto out;
		}
		err = crypto_shash_update(desc, buf, n);
		if (err)
			goto out;
	}

	err = crypto_shash_final(desc, h->bytes);
	if (likely(!err))
		*size = len;

out:
	shash_desc_zero(desc);
free_page:
	free_page((unsigned long)buf);
	return err;
}
