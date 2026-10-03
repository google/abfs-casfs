// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 */
#include <linux/debugfs.h>
#include <linux/idr.h>
#include <linux/fs.h>
#include <linux/parser.h>
#include <linux/namei.h>
#include <linux/statfs.h>
#include <linux/xattr.h>
#include <linux/seq_file.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/moduleparam.h>

#include "casfs.h"
#include "fs.h"
#include "node.h"
#include "compat_dir.h"
#include "compat_fs.h"
#include "sysfs.h"

int casfs_max_mounts = 10;
int casfs_max_git_pools;
int casfs_max_tap_entries = DEFAULT_MAX_TAP_ENTRIES;

module_param(casfs_max_mounts, int, 0444);
MODULE_PARM_DESC(casfs_max_mounts,
		 "Maximum concurrent mount instances allowed for casfs");

module_param(casfs_max_git_pools, int, 0444);
MODULE_PARM_DESC(
	casfs_max_git_pools,
	"Maximum git inflate and deflate objects allowed for casfs control (0 for auto)");

module_param(casfs_max_tap_entries, int, 0444);
MODULE_PARM_DESC(
	casfs_max_tap_entries,
	"Global memory cap ceiling threshold for outstanding tap allocation entries");

static int param_set_read_only(const char *val, const struct kernel_param *kp)
{
	return -EPERM;
}

static int param_get_ro_string(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%s\n", *(const char **)kp->arg);
}

static const struct kernel_param_ops param_ops_ro_string = {
	.set = param_set_read_only,
	.get = param_get_ro_string,
};

/*
 * Virtual/logical interface version implemented by CASFS for ABFS <-> CASFS compatibility.
 */
static const char *casfs_implemented_interface_version = "1.5.0";
module_param_cb(implemented_interface_version, &param_ops_ro_string,
		&casfs_implemented_interface_version, 0444);
MODULE_PARM_DESC(implemented_interface_version,
		 "Virtual interface version implemented by CASFS");

/*
 * Minimum virtual/logical interface version required by CASFS from ABFS.
 */
static const char *casfs_required_interface_version = "1.5.0";
module_param_cb(required_interface_version, &param_ops_ro_string,
		&casfs_required_interface_version, 0444);
MODULE_PARM_DESC(required_interface_version,
		 "Minimum ABFS virtual interface version required by CASFS");

atomic_t casfs_mount_slots = ATOMIC_INIT(0);
DEFINE_IDA(casfs_instance_ida);

static const struct xattr_handler cas_xattr_casfs_handler = {
	.prefix = XATTR_CASFS_PREFIX,
	.get = cas_xattr_get,
	.set = cas_xattr_set,
};

static const struct xattr_handler cas_xattr_user_handler = {
	.prefix = XATTR_USER_CASFS_PREFIX,
	.get = cas_xattr_get,
	.set = cas_xattr_set,
};

const struct xattr_handler *cas_xattr_handlers[] = { &cas_xattr_casfs_handler,
						     &cas_xattr_user_handler,
						     NULL };

static void cas_kill_super(struct super_block *sb)
{
	ktime_t end, start = ktime_get();

	casfs_kill_super(sb);

	end = ktime_get();

	pr_info("Successfully unmounted in %lld milli-seconds\n",
		(end - start) / 1000000);
}

/*
 * Display the mount options in /proc/mounts.
 */
static int cas_show_options(struct seq_file *m, struct dentry *root)
{
	struct cas_fs *fs = SB_TO_FS(root->d_sb);

	seq_printf(m, ",instance=%d", fs->instance_ida);

	if (fs->cache_dir_str && fs->cache_dir_str[0] != '\0') {
		seq_puts(m, ",cache=");
		seq_escape(m, fs->cache_dir_str, " \t\n\\,");
	}

	if (fs->cache.scratch.name) {
		seq_puts(m, ",scratch=");
		seq_escape(m, fs->cache.scratch.name, " \t\n\\,");
	}

	if (fs->cache_socket_str) {
		seq_puts(m, ",socket=");
		seq_escape(m, fs->cache_socket_str, " \t\n\\,");
	}

	if (fs->is_local)
		seq_puts(m, ",local");

	return 0;
}

enum {
	CAS_OPT_CACHE,
	CAS_OPT_LOCAL,
	CAS_OPT_FETCH_TIMEOUT,
	CAS_OPT_SCRATCH,
	CAS_OPT_SOCKET,
	CAS_OPT_ERR,
};

static const struct fs_parameter_spec casfs_fs_parameters[] = {
	CAS_FSPARAM_PATH("cache", CAS_OPT_CACHE),
	fsparam_flag("local", CAS_OPT_LOCAL),
	fsparam_u32("fetch-timeout", CAS_OPT_FETCH_TIMEOUT),
	CAS_FSPARAM_PATH("scratch", CAS_OPT_SCRATCH),
	CAS_FSPARAM_PATH("socket", CAS_OPT_SOCKET),
	{}
};

static void casfs_context_free(struct fs_context *fc)
{
	struct casfs_mount_opts *opts = fc->fs_private;

	if (opts) {
		kfree(opts->cache_dir_str);
		kfree(opts->cache_scratch);
		kfree(opts->cache_socket);
		kfree(opts);
	}
}
static void cas_opt_unescape(char *s)
{
	char *d = s;

	for (;; s++, d++) {
		if (*s == '\\')
			s++;
		*d = *s;
		if (!*s)
			break;
	}
}

static int casfs_parse_string(char **dest, const char *src)
{
	if (*dest)
		return -EEXIST;

	*dest = kstrdup(src, GFP_KERNEL);
	if (!*dest)
		return -ENOMEM;
	cas_opt_unescape(*dest);
	return 0;
}
static int casfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	int opt;
	int ret;
	struct casfs_mount_opts *opts = fc->fs_private;
	struct fs_parse_result result;

	opt = fs_parse(fc, casfs_fs_parameters, param, &result);
	if (opt < 0)
		return opt;

	switch (opt) {
	case CAS_OPT_CACHE:
		ret = casfs_parse_string(&opts->cache_dir_str, param->string);
		if (ret != 0)
			return invalfc(fc, "Failed to set cache dir (%d)", ret);

		break;

	case CAS_OPT_LOCAL:
		opts->is_local = true;
		break;

	case CAS_OPT_FETCH_TIMEOUT:
		opts->fetch_timeout = result.uint_32;
		break;

	case CAS_OPT_SCRATCH:
		ret = casfs_parse_string(&opts->cache_scratch, param->string);
		if (ret != 0)
			return invalfc(fc,
				       "Failed to set cache scratch dir (%d)",
				       ret);

		break;

	case CAS_OPT_SOCKET:
		ret = casfs_parse_string(&opts->cache_socket, param->string);
		if (ret != 0)
			return invalfc(fc, "Failed to set socket path (%d)",
				       ret);
		break;
	}
	return 0;
}

static int casfs_validate_fc(struct fs_context *fc)
{
	struct casfs_mount_opts *opts = fc->fs_private;

	if (!opts->cache_dir_str) {
		pr_err("option cache=<cache-dir> is not specified\n");
		return -EINVAL;
	}
	return 0;
}

static int casfs_get_tree(struct fs_context *fc)
{
	int ret;

	ret = casfs_validate_fc(fc);
	if (ret != 0)
		return ret;

	return get_tree_nodev(fc, cas_fill_super);
}
const struct fs_context_operations casfs_context_ops = {
	.free = casfs_context_free,
	.parse_param = casfs_parse_param,
	.get_tree = casfs_get_tree,
};

static int cas_init_fs_context(struct fs_context *fc)
{
	struct casfs_mount_opts *opts;

	opts = kzalloc(sizeof(struct casfs_mount_opts), GFP_KERNEL);
	if (!opts)
		return -ENOMEM;

	opts->fetch_timeout = 120000;
	fc->ops = &casfs_context_ops;
	fc->fs_private = opts;
	return 0;
}

struct file_system_type cas_fs_type = {
	.owner = THIS_MODULE,
	.name = "casfs",
	.init_fs_context = cas_init_fs_context,
	.parameters = casfs_fs_parameters,
	.kill_sb = cas_kill_super,
};
static void cas_node_ctor(void *data)
{
	struct cas_node *node = data;

	inode_init_once(CAS_TO_INODE(node));
}

int cas_fill_super(struct super_block *sb, struct fs_context *fc)
{
	int err = 0;
	struct cas_fs *fs;
	struct inode *root_ino = NULL;
	const struct cas_hash init_hash = cas_empty_dir_hash;

	struct casfs_mount_opts *opts =
		(struct casfs_mount_opts *)(fc->fs_private);

	char name_buf[64];
	int ida_id;

	if (atomic_dec_if_positive(&casfs_mount_slots) < 0) {
		errorfc(fc,
			"Maximum concurrent casfs mount limit reached (%d max)",
			casfs_max_mounts);
		return -ENOSPC;
	}

	ida_id = ida_alloc_max(&casfs_instance_ida, casfs_max_mounts - 1,
			       GFP_KERNEL);
	if (ida_id < 0) {
		atomic_inc(&casfs_mount_slots);
		return ida_id; // Pass back -ENOMEM or -ENOSPC from the IDA
	}
	fs = kzalloc(sizeof(struct cas_fs), GFP_KERNEL);
	if (unlikely(!fs))
		goto out;

	fs->instance_ida = ida_id;
	sb->s_fs_info = fs;
	atomic64_set(&fs->next_ino, 0ull);
	atomic_set(&fs->inodes_in_use, 0);
	fs->sb = sb;

	fs->fetch_timeout = 120000;
	ktime_get_coarse_real_ts64(&fs->mount_time);

	fs->is_local = opts->is_local;
	fs->cache_dir_str = opts->cache_dir_str;
	fs->fetch_timeout = opts->fetch_timeout;
	opts->cache_dir_str = NULL;
	if (WARN_ON(sb->s_user_ns != current_user_ns())) {
		err = -EIO;
		goto out_free_fs;
	}

	/* Set scratch directory based on instance if not specified by user*/
	if (!opts->cache_scratch) {
		opts->cache_scratch =
			kasprintf(GFP_KERNEL, "mnt_%d", fs->instance_ida);
		if (unlikely(!opts->cache_scratch)) {
			err = -ENOMEM;
			goto out_free_fs;
		}
	}

	err = cas_cache_init(fs->cache_dir_str, &fs->cache,
			     opts->cache_scratch);
	if (unlikely(err)) {
		err = -EINVAL;
		goto out_free_fs;
	}

	err = casfs_sysfs_register(fs, &fs->sysfs);
	if (unlikely(err))
		goto out_destroy_cache;

	if (!fs->is_local) {
		if (opts->cache_socket) {
			fs->cache_socket_str =
				kstrdup(opts->cache_socket, GFP_KERNEL);
		} else {
			fs->cache_socket_str = kasprintf(
				GFP_KERNEL, "%s/socket", fs->cache_dir_str);
		}
		if (unlikely(!fs->cache_socket_str)) {
			err = -ENOMEM;
			goto out_destroy_sysfs;
		}
	}

	/* Non-fatal setup debugfs */
	snprintf(name_buf, sizeof(name_buf), "casfs_%d", fs->instance_ida);

	fs->debugfs_root = debugfs_create_dir(name_buf, NULL);
	if (IS_ERR(fs->debugfs_root)) {
		pr_info("could not create debug interface %pe\n",
			fs->debugfs_root);
		fs->debugfs_root = NULL;
	}

	err = cas_provider_init(&fs->provider, &fs->sysfs, &fs->cache,
				fs->cache_socket_str, fs->fetch_timeout,
				fs->debugfs_root, fs->instance_ida);

	if (unlikely(err))
		goto out_destroy_provider;

	fs->cache_root_inode = d_inode(fs->cache.root_path.dentry);

	snprintf(fs->cache_name, sizeof(fs->cache_name), "casfs_node_%d",
		 fs->instance_ida);
	fs->node_cache = kmem_cache_create(fs->cache_name,
					   sizeof(struct cas_node), 0,
					   CAS_KMEM_CACHE_FLAGS, cas_node_ctor);
	if (!fs->node_cache) {
		err = -ENOMEM;
		goto out_destroy_provider;
	}

	fs->ctrl = cas_ctrl_create(fs->debugfs_root, fs->instance_ida);

	if (unlikely(!fs->ctrl)) {
		err = -ENOMEM;
		goto out_destroy_node_cache;
	}

	sb->s_flags |= SB_NOATIME; /* We do not support access-time */
	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_magic = CASFS_MAGIC;
	sb->s_stack_depth = 0;
	sb->s_maxbytes = CASFS_MAX_FILESIZE;
	sb->s_op = &cas_super_ops;
	casfs_set_default_d_op(sb, &casfs_dentry_operations);
	sb->s_xattr = cas_xattr_handlers;
	sb->s_time_gran = 1;

	root_ino = cas_new_node(sb, CAS_TYPE_DIR, &init_hash, 0, 0);
	if (IS_ERR_OR_NULL(root_ino)) {
		err = PTR_ERR(root_ino) ? PTR_ERR(root_ino) : -ENOMEM;
		goto out_destroy_ctrl;
	}

	sb->s_root = d_make_root(root_ino);
	if (unlikely(!sb->s_root)) {
		err = -ENOMEM;
		goto out_destroy_root;
	}

	err = cas_provider_start(&fs->provider);
	if (unlikely(err))
		goto out_destroy_root;

	fs->ready = true;
	return 0;

out_destroy_root:
	if (sb->s_root)
		dput(sb->s_root);
	/* Note: If d_make_root fails, root_ino is freed inside d_make_root via iput */
out_destroy_ctrl:
	cas_ctrl_destroy(fs->ctrl);
out_destroy_node_cache:
	kmem_cache_destroy(fs->node_cache);
out_destroy_provider:
	cas_provider_destroy(&fs->provider);
out_destroy_sysfs:
	casfs_sysfs_unregister(&fs->sysfs);
out_destroy_cache:
	cas_cache_destroy(&fs->cache);
	debugfs_remove_recursive(fs->debugfs_root);
out_free_fs:
	kfree(fs->cache_dir_str);
	kfree(fs->cache_socket_str);
	kfree(fs);
out:
	ida_free(&casfs_instance_ida, ida_id);
	atomic_inc(&casfs_mount_slots);
	return err == 0 ? -ENOMEM : err;
}

struct inode *cas_alloc_inode(struct super_block *sb)
{
	struct cas_node *node;
	struct cas_fs *fs = SB_TO_FS(sb);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)
	node = kmem_cache_alloc(fs->node_cache, GFP_KERNEL);
#else
	node = alloc_inode_sb(sb, fs->node_cache, GFP_KERNEL);
#endif
	if (!node)
		return NULL;

	pr_debug("new inode allocated\n");
	atomic_inc(&fs->inodes_in_use);
	return CAS_TO_INODE(node);
}

static void cas_free_inode(struct inode *inode)
{
	struct cas_node *node = INODE_TO_CAS(inode);
	struct cas_fs *fs = SB_TO_FS(inode->i_sb);

	pr_debug("freeing inode %llu\n", (unsigned long long)inode->i_ino);
	atomic_dec(&fs->inodes_in_use);
	kmem_cache_free(fs->node_cache, node);
}
void cas_put_super(struct super_block *sb)
{
	struct cas_fs *fs;

	fs = SB_TO_FS(sb);

	fs->ready = false;

	cas_provider_destroy(&fs->provider);

	cas_cache_destroy(&fs->cache);

	kfree(fs->cache_dir_str);
	kfree(fs->cache_socket_str);

	debugfs_remove_recursive(fs->debugfs_root);
	cas_ctrl_destroy(fs->ctrl);

	rcu_barrier();

	kmem_cache_destroy(fs->node_cache);
	casfs_sysfs_unregister(&fs->sysfs);

	ida_free(&casfs_instance_ida, fs->instance_ida);
	atomic_inc(&casfs_mount_slots);

	kfree(fs);

	sb->s_dev = 0;
}

static int cas_statfs(struct dentry *dentry, struct kstatfs *statfs)
{
	int err;
	struct super_block *sb = dentry->d_sb;
	struct cas_fs *fs = SB_TO_FS(sb);

	err = vfs_statfs(&fs->cache.root_path, statfs);
	if (unlikely(err))
		return err;

	statfs->f_type = CASFS_MAGIC;
	statfs->f_namelen = NAME_MAX;

	statfs->f_ffree = statfs->f_files - atomic_read(&fs->inodes_in_use);
	return 0;
}
static int cas_drop_inode(struct inode *inode)
{
	return 1;
}

struct super_operations cas_super_ops = { .alloc_inode = cas_alloc_inode,
					  .free_inode = cas_free_inode,
					  .destroy_inode = cas_destroy_inode,
					  .drop_inode = cas_drop_inode,
					  .put_super = cas_put_super,
					  .show_options = cas_show_options,
					  .statfs = cas_statfs,
					  CAS_SUPER_OPS_REMOUNT() };

int cas_fs_new_blob(struct cas_fs *fs, const struct cas_hash *hash,
		    const void *buf, size_t len)
{
	int err = cas_store_put_bytes(&fs->cache.blobs, hash, buf, len);

	if (likely(!err))
		cas_provider_queue_new_object(&fs->provider, hash,
					      cas_type_blob);

	return err;
}

void cas_fs_root_dir_change(struct cas_fs *fs, const struct cas_hash *hash)
{
	pr_debug("root has a new hash %20phN\n", hash);
}
