/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023 Google LLC
 */

#ifndef _PROVIDER_H_
#define _PROVIDER_H_

#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/un.h>
#include <linux/net.h>
#include <net/sock.h>
#include <linux/socket.h>
#include <linux/mutex.h>
#include <linux/hashtable.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>
#include "cache.h"
#include "hash.h"

/* State flags for tracking forced unmount/shutdown */
#define CAS_PROVIDER_UNMOUNTING 0

enum message_type {
	msg_type_fetch_blob = 0x01,
	msg_type_fetch_blob_resp = 0x02,
	msg_type_fetch_tree = 0x03,
	msg_type_fetch_tree_resp = 0x04,
	msg_type_fetch_object = 0x05,
	msg_type_fetch_object_resp = 0x06,
	msg_type_new_blob = 0x07,
	msg_type_new_tree = 0x08,
	msg_type_new_object = 0x09,
	msg_type_fetch_git_object = 0x0a,
	msg_type_fetch_stat = 0x0b,
	msg_type_fetch_stat_resp = 0x0c,
	msg_type_notify_use = 0x0e,
	msg_type_fetch_ari = 0x17,
	msg_type_fetch_ari_resp = 0x18,
	msg_type_init_deeptree = 0x19,
	msg_type_init_deeptree_resp = 0x1a,
	msg_type_check_status = 0x1b,
	msg_type_check_status_resp = 0x1c,
	// always last
	msg_type_max
};

struct cas_provider {
	bool has_remote;
	bool conn_err_logged;
	struct casfs_sysfs *sysfs;
	atomic_t last_id;
	struct delayed_work poll_work;
	atomic_t upstream_status;
	char socket_path[PATH_MAX];
	struct mutex write_lock;
	struct socket *sock;
	struct sockaddr_un addr;
	struct cas_cache *cache;
	int fetch_timeout;
	struct task_struct *task;
	struct mutex requests_lock;
	DECLARE_HASHTABLE(requests, 14);
	DECLARE_HASHTABLE(ignore_reqs, 14);

	/* Async work management fields */
	unsigned long flags;
	struct workqueue_struct *notify_wq;
	struct kmem_cache *notify_slab; /* Dedicated slab cache */
	char wq_name[32];
	char slab_name[32];

	/* DebugFS Observability Counters */
	atomic64_t msg_counters[msg_type_max];
	u64 stat_socket_retries; /* Retries due to busy sockets (mutex protected) */
	atomic64_t stat_send_err_comm; /* Framing / write / protocol failures */
	atomic64_t stat_send_err_timeout; /* Send queue retries exhausted */
	atomic64_t stat_fetch_timeouts; /* Provider response wait timeouts */
	atomic64_t stat_fetch_failures; /* Provider explicit fetch failures */
};

enum cas_req_obj_type {
	cas_type_blob,
	cas_type_tree,
	cas_type_object,
	cas_type_ari,
};

struct cas_stat_info {
	uint64_t size;
	uint32_t mode;
	enum cas_req_obj_type type;
};

struct cas_provider_file;

int cas_provider_init(struct cas_provider *provider, struct casfs_sysfs *sysfs,
		      struct cas_cache *cache, const char *socket_path,
		      int fetch_timeout, struct dentry *debugfs_parent,
		      int index);

void cas_provider_destroy(struct cas_provider *provider);

int cas_provider_start(struct cas_provider *provider);

void cas_provider_queue_new_object(struct cas_provider *provider,
				   const struct cas_hash *hash,
				   enum cas_req_obj_type type);

void cas_provider_queue_notify_use(struct cas_provider *provider,
				   const struct cas_hash *hash);

int cas_provider_get_blob_path(struct cas_provider *provider,
			       const struct cas_hash *hash, struct path *path);

int cas_provider_get_blob_bytes(struct cas_provider *provider,
				const struct cas_hash *hash, void **read_buf,
				ssize_t *file_size);

int cas_provider_get_blob_size(struct cas_provider *provider,
			       const struct cas_hash *hash, ssize_t *size);

int cas_provider_get_object_bytes(struct cas_provider *provider,
				  const struct cas_hash *hash, void **read_buf,
				  ssize_t *file_size);

int cas_provider_query_tree(struct cas_provider *provider,
			    const struct cas_hash *hash);

int cas_provider_query(struct cas_provider *provider,
		       const struct cas_hash *hash,
		       enum cas_req_obj_type *type);

int cas_provider_query_git(struct cas_provider *provider,
			   const struct cas_hash *hash);

int cas_provider_path(struct cas_provider *provider, enum cas_req_obj_type type,
		      const struct cas_hash *hash, struct path *path);

int cas_provider_ari_path(struct cas_provider *provider, const char *ari,
			  struct path *path);

int cas_provider_tree_paths(struct cas_provider *provider,
			    const struct cas_hash *hash, struct path *path,
			    struct path *meta_path);

int cas_provider_query_stat(struct cas_provider *provider,
			    const struct cas_hash *hash,
			    struct cas_stat_info *stat);

extern struct cas_provider_file *
cas_provider_new_file(struct cas_provider *provider,
		      enum cas_req_obj_type type);
extern int cas_provider_file_write(struct cas_provider_file *file,
				   const char *buf, size_t count, loff_t *pos);
extern int cas_provider_file_close(struct cas_provider_file *file,
				   const struct cas_hash *hash);
extern int cas_provider_file_abort(struct cas_provider_file *file);

int cas_provider_check(struct cas_provider *provider);

#endif /* _PROVIDER_H_ */
