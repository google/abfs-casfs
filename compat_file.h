/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Google LLC
 */

#ifndef _CASFS_COMPAT_FILE_H_
#define _CASFS_COMPAT_FILE_H_

#include <linux/fs.h>
#include <linux/version.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/mm.h>
#include "compat.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
#include <linux/backing-file.h>
#define HAS_BACKING_FILE_OPS 1
#else
#define HAS_BACKING_FILE_OPS 0
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
#define copy_splice_read_fn generic_file_splice_read
#else
#define copy_splice_read_fn copy_splice_read
#endif

#ifndef ITER_DEST
#define ITER_DEST READ
#endif

#ifndef ITER_SOURCE
#define ITER_SOURCE WRITE
#endif

#if HAS_BACKING_FILE_OPS
static inline struct file *cas_backing_file_open(const struct path *user_path,
						 const struct file *user_file,
						 int flags,
						 const struct path *real_path,
						 const struct cred *cred)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 0)
	(void)user_path;
	return backing_file_open(user_file, flags, real_path, cred);
#else
	(void)user_file;
	return backing_file_open(user_path, flags, real_path, cred);
#endif
}
#else
static inline struct file *cas_backing_file_open(const struct path *user_path,
						 const struct file *user_file,
						 int flags,
						 const struct path *real_path,
						 const struct cred *cred)
{
	(void)user_path;
	(void)user_file;
	return dentry_open(real_path, flags, cred);
}
#endif

#ifndef init_kiocb
static inline void cas_init_kiocb(struct kiocb *ki, struct file *filp)
{
	ki->ki_filp = filp;
	ki->ki_pos = filp->f_pos;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
	ki->ki_flags = filp->f_iocb_flags;
#else
	ki->ki_flags = iocb_flags(filp);
#endif

	ki->ki_ioprio = get_current_ioprio();
}
#define init_kiocb cas_init_kiocb
#endif

#ifndef init_sync_kiocb
#define init_sync_kiocb(ki, filp) init_kiocb(ki, filp)
#endif

static inline unsigned long
casfs_mm_get_unmapped_area(struct mm_struct *mm, struct file *file,
			   unsigned long addr, unsigned long len,
			   unsigned long pgoff, unsigned long flags)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 10, 0)
	return mm->get_unmapped_area(file, addr, len, pgoff, flags);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(7, 0, 0)
	return mm_get_unmapped_area(mm, file, addr, len, pgoff, flags);
#else
	return mm_get_unmapped_area(file, addr, len, pgoff, flags);
#endif
}

static inline int cas_compat_mmap(struct file *file, struct file *real_file,
				  struct vm_area_struct *vma)
{
#if HAS_BACKING_FILE_OPS
	struct backing_file_ctx ctx = {
		.cred = current_cred(),
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
		.user_file = file,
#endif
		.accessed = NULL,
	};
	return backing_file_mmap(real_file, vma, &ctx);
#else
	(void)file;
	get_file(real_file);
	fput(vma->vm_file);
	vma->vm_file = real_file;

	return call_mmap(real_file, vma);
#endif
}

static inline ssize_t cas_compat_read_iter(struct file *file,
					   struct file *real_file,
					   struct iov_iter *iter,
					   struct kiocb *iocb)
{
#if HAS_BACKING_FILE_OPS
	struct backing_file_ctx ctx = {
		.cred = current_cred(),
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
		.user_file = file,
#endif
		.accessed = NULL,
	};
	return backing_file_read_iter(real_file, iter, iocb, iocb->ki_flags,
				      &ctx);
#else
	struct file *old_file = iocb->ki_filp;
	ssize_t ret;

	(void)file;
	iocb->ki_filp = real_file;
	ret = vfs_iter_read(real_file, iter, &iocb->ki_pos, iocb->ki_flags);
	iocb->ki_filp = old_file;

	return ret;
#endif
}

static inline ssize_t cas_compat_write_iter(struct file *file,
					    struct file *real_file,
					    struct iov_iter *iter,
					    struct kiocb *iocb)
{
#if HAS_BACKING_FILE_OPS
	struct backing_file_ctx ctx = {
		.cred = current_cred(),
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
		.user_file = file,
#endif
		.end_write = NULL,
	};
	return backing_file_write_iter(real_file, iter, iocb, iocb->ki_flags,
				       &ctx);
#else
	struct file *old_file = iocb->ki_filp;
	ssize_t ret;

	(void)file;
	iocb->ki_filp = real_file;
	ret = vfs_iter_write(real_file, iter, &iocb->ki_pos, iocb->ki_flags);
	iocb->ki_filp = old_file;

	return ret;
#endif
}

static inline ssize_t cas_compat_splice_read(struct file *in,
					     struct file *real_in, loff_t *ppos,
					     struct pipe_inode_info *pipe,
					     size_t len, unsigned int flags)
{
#if HAS_BACKING_FILE_OPS
	struct backing_file_ctx ctx = {
		.cred = current_cred(),
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
		.user_file = in,
#endif
		.accessed = NULL,
	};
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	struct kiocb iocb;
	ssize_t ret;

	init_kiocb(&iocb, in);
	iocb.ki_pos = ppos ? *ppos : 0;

	ret = backing_file_splice_read(real_in, &iocb, pipe, len, flags, &ctx);

	if (ret > 0 && ppos)
		*ppos = iocb.ki_pos;
	return ret;
#else
	return backing_file_splice_read(real_in, ppos, pipe, len, flags, &ctx);
#endif

#else /* !HAS_BACKING_FILE_OPS */
	return copy_splice_read_fn(in, ppos, pipe, len, flags);
#endif
}

static inline ssize_t cas_compat_splice_write(struct pipe_inode_info *pipe,
					      struct file *out,
					      struct file *real_out,
					      loff_t *ppos, size_t len,
					      unsigned int flags)
{
#if HAS_BACKING_FILE_OPS
	struct backing_file_ctx ctx = {
		.cred = current_cred(),
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
		.user_file = out,
#endif
		.end_write = NULL,
	};
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	struct kiocb iocb;
	ssize_t ret;

	init_sync_kiocb(&iocb, out);
	iocb.ki_pos = ppos ? *ppos : 0;

	ret = backing_file_splice_write(pipe, real_out, &iocb, len, flags,
					&ctx);

	if (ret > 0 && ppos)
		*ppos = iocb.ki_pos;
	return ret;
#else
	return backing_file_splice_write(pipe, real_out, ppos, len, flags,
					 &ctx);
#endif

#else /* !HAS_BACKING_FILE_OPS */
	struct kiocb iocb;
	ssize_t ret;

	init_sync_kiocb(&iocb, out);
	iocb.ki_pos = ppos ? *ppos : 0;

	ret = iter_file_splice_write(pipe, real_out, &iocb.ki_pos, len, flags);

	if (ret > 0 && ppos)
		*ppos = iocb.ki_pos;
	return ret;
#endif
}

#endif /* _CASFS_COMPAT_FILE_H_ */
