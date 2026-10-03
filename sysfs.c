// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Google LLC
 */
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/completion.h>
#include "fs.h"
#include "provider.h"
#include "sysfs.h"
#include "compat.h"

static struct kobject *casfs_kobj;

/*
 * SysFS Attribute Framework
 */
struct casfs_attr {
	struct attribute attr;
	ssize_t (*show)(struct cas_fs *fs, char *buf);
	ssize_t (*store)(struct cas_fs *fs, const char *buf, size_t count);
};

#define CASFS_ATTR_RO(_name) \
	static struct casfs_attr casfs_attr_##_name = __ATTR_RO(_name)

#define CASFS_ATTR_RW(_name) \
	static struct casfs_attr casfs_attr_##_name = __ATTR_RW(_name)

/*
 * Attribute Show/Store Handlers
 */
static ssize_t upstream_status_show(struct cas_fs *fs, char *buf)
{
	int status;

	if (fs->is_local)
		return sysfs_emit(buf, "local (0)\n");

	status = cas_provider_check(&fs->provider);
	if (status == 0)
		return sysfs_emit(buf, "connected (0)\n");

	return sysfs_emit(buf, "disconnected (%d: %pe)\n", status,
			  ERR_PTR(status));
}
CASFS_ATTR_RO(upstream_status);

/*
 * Central Attribute Table
 */
static struct attribute *casfs_attrs[] = {
	&casfs_attr_upstream_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(casfs);

/*
 * Kobject Operations and Type Definition
 */
static ssize_t casfs_attr_show(struct kobject *kobj, struct attribute *attr,
			       char *buf)
{
	struct casfs_attr *cattr = container_of(attr, struct casfs_attr, attr);
	struct casfs_sysfs *sysfs =
		container_of(kobj, struct casfs_sysfs, kobj);

	if (!cattr->show)
		return -EIO;
	return cattr->show(sysfs->fs, buf);
}

static const struct sysfs_ops casfs_sysfs_ops = {
	.show = casfs_attr_show,
};

static void casfs_kobj_release(struct kobject *kobj)
{
	struct casfs_sysfs *sysfs =
		container_of(kobj, struct casfs_sysfs, kobj);

	complete(&sysfs->kobj_unregister);
}

static const struct kobj_type casfs_ktype = {
	.release = casfs_kobj_release,
	.sysfs_ops = &casfs_sysfs_ops,
	.default_groups = casfs_groups,
};

/*
 * Public API
 */
int casfs_sysfs_module_init(void)
{
	casfs_kobj = kobject_create_and_add("casfs", fs_kobj);
	if (!casfs_kobj) {
		pr_warn("casfs: failed to create /sys/fs/casfs\n");
		return -ENOMEM;
	}
	return 0;
}

void casfs_sysfs_module_exit(void)
{
	kobject_put(casfs_kobj);
	casfs_kobj = NULL;
}

int casfs_sysfs_register(struct cas_fs *fs, struct casfs_sysfs *sysfs)
{
	int err;

	if (!casfs_kobj)
		return 0;

	sysfs->fs = fs;
	init_completion(&sysfs->kobj_unregister);

	err = kobject_init_and_add(&sysfs->kobj, casfs_ktype_ptr(&casfs_ktype),
				   casfs_kobj, "%d", fs->instance_ida);
	if (unlikely(err)) {
		pr_warn("casfs: failed to create sysfs directory for instance %d: %d\n",
			fs->instance_ida, err);
		kobject_put(&sysfs->kobj);
		return err;
	}

	return 0;
}

void casfs_sysfs_unregister(struct casfs_sysfs *sysfs)
{
	if (casfs_kobj && sysfs->kobj.state_in_sysfs) {
		kobject_del(&sysfs->kobj);
		kobject_put(&sysfs->kobj);
		wait_for_completion(&sysfs->kobj_unregister);
	}
}

void casfs_sysfs_notify_upstream_status(struct casfs_sysfs *sysfs)
{
	if (sysfs && casfs_kobj && sysfs->kobj.state_in_sysfs)
		sysfs_notify(&sysfs->kobj, NULL, "upstream_status");
}
