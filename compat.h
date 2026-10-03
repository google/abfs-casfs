/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024 Google LLC
 */

#ifndef _CASFS_COMPAT_H_
#define _CASFS_COMPAT_H_

#include <linux/version.h>
#include <linux/fs.h>
#include <linux/uio.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
#include <linux/user_namespace.h>
#define mnt_idmap_type user_namespace
#define casfs_nop_idmap (&init_user_ns)
#define nop_mnt_idmap init_user_ns
#define mnt_idmap_fn mnt_user_ns
#define file_mnt_idmap_fn file_mnt_user_ns
#define SET_MNT_IDMAPS(x) .old_mnt_userns = x, .new_mnt_userns = x
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 18, 0)
#define mnt_idmap_type mnt_idmap
#define casfs_nop_idmap (&nop_mnt_idmap)
#define mnt_idmap_fn mnt_idmap
#define file_mnt_idmap_fn file_mnt_idmap
#define SET_MNT_IDMAPS(x) .old_mnt_idmap = x, .new_mnt_idmap = x
#else
#define mnt_idmap_type mnt_idmap
#define casfs_nop_idmap (&nop_mnt_idmap)
#define mnt_idmap_fn mnt_idmap
#define file_mnt_idmap_fn file_mnt_idmap
#define SET_MNT_IDMAPS(x) .mnt_idmap = x
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
#define tmpfile_arg_type dentry
#else
#define tmpfile_arg_type file
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
#define filemap_splice_read_fn generic_file_splice_read
#else
#define filemap_splice_read_fn filemap_splice_read
#endif

static inline struct dentry *casfs_lookup_one(struct mnt_idmap_type *idmap,
					      const char *name,
					      struct dentry *base, size_t len)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 16, 0)
	return lookup_one(idmap, name, base, len);
#else
	return lookup_one(idmap, &QSTR_LEN(name, len), base);
#endif
}

static inline struct dentry *
casfs_lookup_one_unlocked(struct mnt_idmap_type *idmap, const char *name,
			  struct dentry *base, size_t len)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0)
	return lookup_one_len_unlocked(name, base, len);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(6, 16, 0)
	return lookup_one_unlocked(idmap, name, base, len);
#else
	return lookup_one_unlocked(idmap, &QSTR_LEN(name, len), base);
#endif
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 9, 10)
#define CAS_KMEM_CACHE_FLAGS \
	(SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD | SLAB_ACCOUNT)
#else
#define CAS_KMEM_CACHE_FLAGS (SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0)
/**
 * list_next_entry_circular - get the next element in list
 * @pos:	the type * to cursor.
 * @head:	the list head to take the element from.
 * @member:	the name of the list_head within the struct.
 */
#define list_next_entry_circular(pos, head, member)               \
	(list_is_last(&(pos)->member, head) ?                     \
		 list_first_entry(head, typeof(*(pos)), member) : \
		 list_next_entry(pos, member))
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 17, 0)
#define SET_OLD_PARENT(x) .old_dir = d_inode(x->d_parent)
#define SET_NEW_PARENT(x) .new_dir = d_inode(x->d_parent)
#else
#define SET_OLD_PARENT(x) .old_parent = x->d_parent
#define SET_NEW_PARENT(x) .new_parent = x->d_parent
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
#define CAS_SOCKADDR_CONNECT_CAST(addr) ((struct sockaddr_unsized *)(addr))
#else
#define CAS_SOCKADDR_CONNECT_CAST(addr) ((struct sockaddr *)(addr))
#endif

static inline void casfs_d_instantiate_persistent(struct dentry *dentry,
						  struct inode *inode)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
	d_make_persistent(dentry, inode);
#else
	if (d_unhashed(dentry))
		d_add(dentry, inode);
	else
		d_instantiate(dentry, inode);
	dget(dentry);
#endif
}

static inline void casfs_dentry_unpin_on_remove(struct dentry *dentry)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
	d_make_discardable(dentry);
#else
	dput(dentry);
#endif
}

static inline struct kobj_type *casfs_ktype_ptr(const struct kobj_type *ktype)
{
	return (void *)ktype;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
/* Linux 6.12+ signature: kvrealloc(p, new_size, flags) */
static inline void *casfs_kvrealloc(void *p, size_t old_size, size_t new_size,
				    gfp_t flags)
{
	return kvrealloc(p, new_size, flags);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
/* Linux 6.1 to 6.11 signature: kvrealloc(p, old_size, new_size, flags) */
static inline void *casfs_kvrealloc(void *p, size_t old_size, size_t new_size,
				    gfp_t flags)
{
	return kvrealloc(p, old_size, new_size, flags);
}
#else
/* Fallback for pre-6.1 kernels lacking native kvrealloc */
static inline void *casfs_kvrealloc(void *p, size_t old_size, size_t new_size,
				    gfp_t flags)
{
	void *new_p;

	if (new_size == 0) {
		kvfree(p);
		return NULL;
	}

	new_p = kvmalloc(new_size, flags);
	if (!new_p)
		return NULL;

	if (p) {
		size_t copy_sz = min(old_size, new_size);

		memcpy(new_p, p, copy_sz);
		kvfree(p);
	}

	return new_p;
}
#endif

#endif /* _CASFS_COMPAT_H_ */
