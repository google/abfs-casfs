// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 */

#include <linux/debugfs.h>
#include <linux/dcache.h>
#include <linux/file.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/skbuff.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/key.h>
#include <linux/parser.h>
#include <linux/fs_stack.h>
#include <linux/slab.h>
#include <linux/magic.h>
#include <linux/list.h>
#include <linux/poll.h>

#include "ctrl.h"
#include "tap.h"
#include "session.h"

static int debugfs_histo_show(struct seq_file *m, void *v)
{
	struct tap_ctrl *ctrl = m->private;

	mutex_lock(&ctrl->lock);

	seq_puts(m, "--- Tap Queue Depth Distribution HWM Summary ---\n");
	seq_printf(m, "0 - 64       items : %llu\n",
		   ctrl->peak_depth_histogram[0]);
	seq_printf(m, "65 - 256     items : %llu\n",
		   ctrl->peak_depth_histogram[1]);
	seq_printf(m, "257 - 1K     items : %llu\n",
		   ctrl->peak_depth_histogram[2]);
	seq_printf(m, "1K - 4K      items : %llu\n",
		   ctrl->peak_depth_histogram[3]);
	seq_printf(m, "4K - 16K     items : %llu\n",
		   ctrl->peak_depth_histogram[4]);
	seq_printf(m, "16K - 67K    items : %llu\n",
		   ctrl->peak_depth_histogram[5]);
	seq_printf(m, "67K - 262K   items : %llu\n",
		   ctrl->peak_depth_histogram[6]);
	seq_printf(m, "262K - 1024K items : %llu\n",
		   ctrl->peak_depth_histogram[7]);
	seq_printf(m, "> 1024K      items : %llu\n",
		   ctrl->peak_depth_histogram[8]);

	seq_printf(m, "Absolute Peak Single Session HWM: %llu\n",
		   ctrl->peak_per_session_hwm);
	seq_printf(m, "Mount-Wide HWM: %d\n",
		   atomic_read(&ctrl->allocated_hwm));

	mutex_unlock(&ctrl->lock);
	return 0;
}

static int debugfs_histo_open(struct inode *inode, struct file *file)
{
	return single_open(file, debugfs_histo_show, inode->i_private);
}

static ssize_t debugfs_histo_write(struct file *file,
				   const char __user *user_buf, size_t count,
				   loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct tap_ctrl *ctrl = m->private;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	mutex_lock(&ctrl->lock);
	memset(ctrl->peak_depth_histogram, 0,
	       sizeof(ctrl->peak_depth_histogram));
	ctrl->peak_per_session_hwm = 0;
	mutex_unlock(&ctrl->lock);

	return count;
}
static const struct file_operations fops_histo = {
	.owner = THIS_MODULE,
	.open = debugfs_histo_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = debugfs_histo_write,
};

static int debugfs_atomic64_get(void *data, u64 *val)
{
	*val = atomic64_read((atomic64_t *)data);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(fops_atomic64_ro, debugfs_atomic64_get, NULL,
			 "%llu\n");

static int debugfs_atomic_get(void *data, u64 *val)
{
	*val = atomic_read((atomic_t *)data);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(fops_atomic_ro, debugfs_atomic_get, NULL, "%llu\n");

static void tap_add_debugfs(struct tap_ctrl *ctrl, struct dentry *parent)
{
	struct dentry *tap_dir;

	// Create a sub-folder named "tap" inside the parent instance directory
	tap_dir = debugfs_create_dir("tap", parent);
	if (IS_ERR(tap_dir))
		return;

	debugfs_create_file_unsafe("paths_total", 0444, tap_dir,
				   &ctrl->paths_total, &fops_atomic64_ro);
	debugfs_create_file_unsafe("paths_fast_stack", 0444, tap_dir,
				   &ctrl->paths_fast_stack, &fops_atomic64_ro);
	debugfs_create_file_unsafe("paths_slow_heap", 0444, tap_dir,
				   &ctrl->paths_slow_heap, &fops_atomic64_ro);

	debugfs_create_file("queue_depth_histogram", 0644, tap_dir, ctrl,
			    &fops_histo);

	debugfs_create_file_unsafe("session_overflows", 0444, tap_dir,
				   &ctrl->session_overflows, &fops_atomic_ro);
}

/*
 *
 * Initialize the tap filesystem, creating a "tap" directory
 * under the input dentry root.
 *
 */
struct tap_ctrl *tap_init(struct dentry *parent, int index)
{
	struct tap_ctrl *ctrl = kzalloc(sizeof(struct tap_ctrl), GFP_KERNEL);

	if (unlikely(!ctrl))
		goto out_err;

	snprintf(ctrl->cache_name, sizeof(ctrl->cache_name), "casfs_tap_%d",
		 index);

	mutex_init(&ctrl->lock);
	hash_init(ctrl->sessions);

	ctrl->cache =
		kmem_cache_create(ctrl->cache_name, sizeof(struct tap_data), 0,
				  SLAB_HWCACHE_ALIGN | SLAB_ACCOUNT, NULL);

	if (unlikely(!ctrl->cache))
		goto out_free_ctrl;

	if (parent)
		tap_add_debugfs(ctrl, parent);

	return ctrl;

out_free_ctrl:
	kfree(ctrl);

out_err:
	return NULL;
}

void tap_destroy(struct tap_ctrl *ctrl)
{
	if (!ctrl)
		return;

	rcu_barrier();
	kmem_cache_destroy(ctrl->cache);
	kfree(ctrl);
}
static int tap_data_path_set(struct tap_ctrl *ctrl, struct tap_data_path *p,
			     struct dentry *d)
{
	char stack_buf[512];
	char *tmp_buf = stack_buf;
	char *res_path;
	int rc = 0;

	if (p->buf)
		return -EINVAL;

	atomic64_inc(&ctrl->paths_total);

	res_path = dentry_path_raw(d, tmp_buf, sizeof(stack_buf));
	if (IS_ERR(res_path) && PTR_ERR(res_path) == -ENAMETOOLONG) {
		atomic64_inc(&ctrl->paths_slow_heap);
		tmp_buf = kmalloc(PATH_MAX, GFP_KERNEL_ACCOUNT);
		if (!tmp_buf)
			return -ENOMEM;
		res_path = dentry_path_raw(d, tmp_buf, PATH_MAX);
	} else if (!IS_ERR(res_path)) {
		atomic64_inc(&ctrl->paths_fast_stack);
	}

	if (IS_ERR(res_path)) {
		rc = PTR_ERR(res_path);
		goto out_free_tmp;
	}

	p->buf = kstrdup(res_path, GFP_KERNEL_ACCOUNT);
	if (tmp_buf != stack_buf)
		kfree(tmp_buf);

	if (!p->buf)
		return -ENOMEM;

	p->start = p->buf;
	return 0;

out_free_tmp:
	if (tmp_buf != stack_buf)
		kfree(tmp_buf);
	return rc;
}

static void tap_data_path_free(struct tap_ctrl *ctrl, struct tap_data_path *p)
{
	kfree(p->buf);
	p->buf = NULL;
	p->start = NULL;
}

int tap_data_add_aux_path(struct tap_data *data, struct dentry *d)
{
	return tap_data_path_set(data->ctrl, &data->aux_path, d);
}
int tap_data_add_path(struct tap_data *data, struct dentry *d)
{
	return tap_data_path_set(data->ctrl, &data->path, d);
}
struct tap_data *tap_data_alloc(struct tap_ctrl *ctrl)
{
	struct tap_data *out;
	int current_allocated;
	int old_hwm;

	out = kmem_cache_zalloc(ctrl->cache, GFP_KERNEL);
	if (unlikely(!out))
		return NULL;

	current_allocated = atomic_inc_return(&ctrl->allocated);

	old_hwm = atomic_read_acquire(&ctrl->allocated_hwm);

	while (current_allocated > old_hwm) {
		if (atomic_try_cmpxchg(&ctrl->allocated_hwm, &old_hwm,
				       current_allocated))
			break;

		/*
		 * CAS failed: old_hwm has been automatically updated with the
		 * current value in memory. Under heavy contention, yield the CPU
		 * pipeline briefly to reduce cache-line bouncing on the bus.
		 */
		cpu_relax();
	}

	out->ctrl = ctrl;
	return out;
}

void tap_data_free(struct tap_data *data)
{
	tap_data_path_free(data->ctrl, &data->path);
	tap_data_path_free(data->ctrl, &data->aux_path);

	if (data->is_ari)
		kfree(data->ari);

	atomic_dec(&data->ctrl->allocated);
	kmem_cache_free(data->ctrl->cache, data);
}
struct session *tap_session_get(struct tap_ctrl *ctrl, uint32_t sid)
{
	struct session *s = NULL;

	mutex_lock(&ctrl->lock);
	hash_for_each_possible(ctrl->sessions, s, node, sid) {
		if (s->sid == sid) {
			kref_get(&s->refcount);
			goto out;
		}
	}
	s = NULL;
out:
	mutex_unlock(&ctrl->lock);

	return s;
}
