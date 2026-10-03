// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 */
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/kernel.h>

#include "casfs.h"
#include "fs.h"
#include "version.h"
#include "sysfs.h"

struct casfs_module_context {
	struct dentry *debugfs_root;
};
static struct casfs_module_context casfs_ctx;

static ssize_t debugfs_max_entries_write(struct file *file,
					 const char __user *user_buf,
					 size_t count, loff_t *ppos)
{
	char kbuf[32];
	int new_limit;

	if (count >= sizeof(kbuf))
		return -EINVAL;

	if (copy_from_user(kbuf, user_buf, count))
		return -EFAULT;

	kbuf[count] = '\0';
	if (kstrtoint(kbuf, 10, &new_limit) != 0 || new_limit <= 0)
		return -EINVAL;

	/* Explicitly update the global tracker safely via root debugging action */
	casfs_max_tap_entries = new_limit;
	return count;
}

static const struct file_operations fops_global_max_entries = {
	.write = debugfs_max_entries_write,
	.open = simple_open,
};

static int __init casfs_init(void)
{
	int ret;

	if (casfs_max_mounts <= 0) {
		pr_err("casfs: Invalid max_mounts value (%d). Resetting to default 512.\n",
		       casfs_max_mounts);
		casfs_max_mounts = 512;
	}

	if (casfs_max_tap_entries <= 0) {
		pr_err("casfs: Invalid max_tap_entries value (%d). Resetting to default %d.\n",
		       casfs_max_tap_entries, DEFAULT_MAX_TAP_ENTRIES);
		casfs_max_tap_entries = DEFAULT_MAX_TAP_ENTRIES;
	}

	if (casfs_max_git_pools <= 0)
		casfs_max_git_pools = min_t(unsigned int, num_online_cpus(), 8);

	ret = cas_hash_init();
	if (ret)
		return ret;

	atomic_set(&casfs_mount_slots, casfs_max_mounts);

	memset(&casfs_ctx, 0, sizeof(struct casfs_module_context));

	casfs_ctx.debugfs_root = debugfs_create_dir("casfs", NULL);
	if (!IS_ERR_OR_NULL(casfs_ctx.debugfs_root)) {
		debugfs_create_file("max_tap_entries_override", 0200,
				    casfs_ctx.debugfs_root, NULL,
				    &fops_global_max_entries);
	} else {
		pr_warn("casfs: failed to initialize structural debugfs context paths\n");
		casfs_ctx.debugfs_root = NULL;
	}

	casfs_sysfs_module_init();

	ret = register_filesystem(&cas_fs_type);
	if (unlikely(ret)) {
		pr_err("Failed to register. Error code: %d\n", ret);
		goto out_cleanup;
	}

	pr_notice("Successfully registered\n");
	return 0;

out_cleanup:
	ida_destroy(&casfs_instance_ida);
	debugfs_remove_recursive(casfs_ctx.debugfs_root);
	casfs_sysfs_module_exit();
	cas_hash_exit();
	return ret;
}

static void __exit casfs_exit(void)
{
	int ret;

	ret = unregister_filesystem(&cas_fs_type);

	rcu_barrier();

	ida_destroy(&casfs_instance_ida);

	debugfs_remove_recursive(casfs_ctx.debugfs_root);
	casfs_sysfs_module_exit();

	cas_hash_exit();

	if (likely(ret == 0))
		pr_notice("Successfully unregistered\n");
	else
		pr_err("Failed to unregister. Error code: %d\n", ret);
}

module_init(casfs_init);
module_exit(casfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Google LLC");
MODULE_VERSION(CASFS_VERSION);
MODULE_DESCRIPTION("Content Addressable Filesystem");
