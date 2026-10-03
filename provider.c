// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Google LLC
 */
#include <linux/debugfs.h>
#include <linux/version.h>
#include <net/sock.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

#include <linux/socket.h>
#include <linux/time_types.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include "provider.h"
#include "casfs.h"
#include "sysfs.h"

struct msg_stat_entry {
	enum message_type type;
	const char *name;
	const char *hex;
};

static const struct msg_stat_entry msg_stat_table[] = {
	{ msg_type_fetch_blob, "fetch_blob", "0x01" },
	{ msg_type_fetch_blob_resp, "fetch_blob_resp", "0x02" },
	{ msg_type_fetch_tree, "fetch_tree", "0x03" },
	{ msg_type_fetch_tree_resp, "fetch_tree_resp", "0x04" },
	{ msg_type_fetch_object, "fetch_object", "0x05" },
	{ msg_type_fetch_object_resp, "fetch_object_resp", "0x06" },
	{ msg_type_new_blob, "new_blob", "0x07" },
	{ msg_type_new_tree, "new_tree", "0x08" },
	{ msg_type_new_object, "new_object", "0x09" },
	{ msg_type_fetch_git_object, "fetch_git_object", "0x0a" },
	{ msg_type_fetch_stat, "fetch_stat", "0x0b" },
	{ msg_type_fetch_stat_resp, "fetch_stat_resp", "0x0c" },
	{ msg_type_notify_use, "notify_use", "0x0e" },
	{ msg_type_fetch_ari, "fetch_ari", "0x17" },
	{ msg_type_fetch_ari_resp, "fetch_ari_resp", "0x18" },
	{ msg_type_check_status, "check_status", "0x1b" },
	{ msg_type_check_status_resp, "check_status_resp", "0x1c" },
};

static int cas_provider_new_object(struct cas_provider *provider,
				   const struct cas_hash *hash,
				   enum cas_req_obj_type type);

static int cas_provider_notify_use(struct cas_provider *provider,
				   const struct cas_hash *hash);

static void cas_provider_poll_worker(struct work_struct *work);

// Based on the input error and provider state determine
// if user should query provider
static int try_provider(struct cas_provider *provider, int err)
{
	if (!provider->has_remote || (err != -ENOENT))
		return err;

	return atomic_read(&provider->upstream_status);
}

#define SEND_MESSAGE_TRIES 600 // Up to 30 seconds
#define PROVIDER_TIMEOUT_SEC 1
#define CAS_PROVIDER_POLL_INTERVAL_HZ (5 * HZ)
#define CAS_PROVIDER_STATUS_TIMEOUT_MS 4000

enum cas_notification_action {
	CAS_NOTIFY_ACTION_USE,
	CAS_NOTIFY_ACTION_NEW,
};

/* Deferred notification container structure */
struct cas_notify_work {
	struct work_struct work;
	struct cas_provider *provider;
	struct cas_hash hash;
	enum cas_req_obj_type object_type;
	enum cas_notification_action action;
};

// Allow files to be written through the provider
struct cas_provider_file {
	struct cas_provider *provider;
	struct file *filp;
	struct cas_store *store;
	enum cas_req_obj_type type;
};

enum fetch_result {
	fetch_wait = 0xff,
	fetch_success = 0x01,
	fetch_not_found = 0x02,
	fetch_failed = 0x03,
	fetch_timeout = 0x04,
};

enum cas_resp_obj_type {
	resp_blob = 0x01,
	resp_obj = 0x02,
};

struct cas_provider_request {
	struct hlist_node node;
	unsigned int id;
	byte_t type;
	const struct cas_hash *hash;
	const char *ari;
	wait_queue_head_t wait;
	enum fetch_result result;
	uint64_t size;
	uint32_t mode;
	enum cas_req_obj_type resp_type;
	int error_code;
};

struct cas_provider_ignore {
	struct hlist_node node;
	unsigned int id;
};
const int _MIN_PROVIDER_RESP_LENGTH = 6;
const int _STAT_RESP_LENGTH = 19;
const int _STATUS_RESP_LENGTH = 10;

static inline char cas_type_to_new_object(enum cas_req_obj_type type)
{
	switch (type) {
	case cas_type_blob:
		return msg_type_new_blob;
	case cas_type_tree:
		return msg_type_new_tree;
	case cas_type_object:
		return msg_type_new_object;
	default:
		pr_err("unknown object type %02x", type);
		return -1;
	}
}

static inline char cas_type_to_fetch(enum cas_req_obj_type type)
{
	switch (type) {
	case cas_type_blob:
		return msg_type_fetch_blob;
	case cas_type_tree:
		return msg_type_fetch_tree;
	case cas_type_object:
		return msg_type_fetch_object;
	case cas_type_ari:
		return msg_type_fetch_ari;
	default:
		pr_err("unknown object type %02x", type);
		return -1;
	}
}

static inline char *cas_type_to_string(enum cas_req_obj_type type)
{
	switch (type) {
	case cas_type_blob:
		return "blob";
	case cas_type_tree:
		return "tree";
	case cas_type_object:
		return "object";
	case cas_type_ari:
		return "ari";
	default:
		pr_err("unknown object type %02x", type);
		return "unknown";
	}
}

static inline char *msg_type_to_string(enum message_type type)
{
	switch (type) {
	case msg_type_fetch_blob:
		return "blob";
	case msg_type_fetch_tree:
		return "tree";
	case msg_type_fetch_object:
		return "object";
	case msg_type_fetch_stat:
		return "stat";
	case msg_type_fetch_ari:
		return "ari";
	case msg_type_check_status:
		return "check_status";
	default:
		pr_err("unknown object type %02x", type);
		return "unknown";
	}
}

static inline int cas_provider_error(int err)
{
	switch (err) {
	case 0:
	case -ERESTARTSYS:
	case -EINTR:
	case -ENOMEM:
	case -ENOENT:
	case -ETIMEDOUT:
	case -ENOKEY:
	case -ENOTCONN:
	case -ENETDOWN:
		return err;
	default:
		return -EIO;
	}
}

static inline void
cas_provider_set_upstream_status(struct cas_provider *provider, int status)
{
	int old_status = atomic_xchg(&provider->upstream_status, status);

	if (old_status != status) {
		casfs_sysfs_notify_upstream_status(provider->sysfs);
		if (likely(!test_bit(CAS_PROVIDER_UNMOUNTING,
				     &provider->flags)))
			pr_err("provider status changed from %d to %d\n",
			       old_status, status);
	}
}

static void cas_provider_abort_all_requests(struct cas_provider *provider,
					    enum fetch_result result)
{
	struct cas_provider_request *req;
	struct hlist_node *tmp;
	unsigned int bkt;

	mutex_lock(&provider->requests_lock);
	hash_for_each_safe(provider->requests, bkt, tmp, req, node) {
		if (req->result == fetch_wait) {
			req->result = result;
			wake_up(&req->wait);
		}
	}
	mutex_unlock(&provider->requests_lock);
}

static inline void cas_provider_close(struct cas_provider *provider, int error,
				      bool abort_requests)
{
	if (provider->sock) {
		sock_release(provider->sock);
		provider->sock = NULL;
	}

	if (abort_requests)
		cas_provider_abort_all_requests(provider, fetch_failed);

	switch (error) {
	case 0:
	case -EPROTO:
		break;
	default:
		cas_provider_set_upstream_status(provider, -ENETDOWN);
		break;
	}
}

static int cas_provider_connect(struct cas_provider *provider)
{
	int err;
	int old_sndbuf;
	int sndbuf;
	struct socket *sock;
	struct sockaddr_un *addr = &provider->addr;
	struct __kernel_sock_timeval tv = { .tv_sec = PROVIDER_TIMEOUT_SEC,
					    .tv_usec = 0 };

	if (unlikely(test_bit(CAS_PROVIDER_UNMOUNTING, &provider->flags)))
		return -ESHUTDOWN;

	if (!READ_ONCE(provider->conn_err_logged))
		pr_info("trying to connect to unix domain socket for the cache %s\n",
			provider->socket_path);

	err = sock_create(AF_UNIX, SOCK_STREAM, 0, &provider->sock);
	if (unlikely(err))
		return err;

	sock = provider->sock;
	old_sndbuf = sock->sk->sk_sndbuf;

	err = sock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO_NEW,
			      KERNEL_SOCKPTR(&tv), sizeof(tv));
	if (unlikely(err)) {
		pr_info("failed to set socket options in provider %pe\n",
			ERR_PTR(err));
		cas_provider_close(provider, err, false);
		return err;
	}

	sndbuf = old_sndbuf;
	err = sock_setsockopt(sock, SOL_SOCKET, SO_SNDBUF,
			      KERNEL_SOCKPTR(&sndbuf), sizeof(sndbuf));

	if (unlikely(err)) {
		pr_info("casfs: failed to double provider socket sndbuf: %pe (using default %d bytes)\n",
			ERR_PTR(err), old_sndbuf);
	}
	err = sock->ops->connect(sock, CAS_SOCKADDR_CONNECT_CAST(addr),
				 sizeof(*addr), 0);
	if (unlikely(err)) {
		if (!READ_ONCE(provider->conn_err_logged)) {
			pr_err("cannot connect to unix domain socket for the cache %s\n",
			       provider->socket_path);
			WRITE_ONCE(provider->conn_err_logged, true);
		}
		cas_provider_close(provider, err, false);
	} else {
		cas_provider_set_upstream_status(provider, 0);
		if (READ_ONCE(provider->conn_err_logged)) {
			pr_info("successfully reconnected to unix domain socket for the cache %s\n",
				provider->socket_path);
			WRITE_ONCE(provider->conn_err_logged, false);
		} else {
			pr_info("successfully connected to unix domain socket for the cache %s\n",
				provider->socket_path);
		}
	}

	return err;
}

static void cas_provider_process_stat_msg(struct cas_provider *provider,
					  unsigned int id,
					  enum fetch_result result,
					  uint64_t size, uint32_t mode,
					  enum cas_resp_obj_type t)
{
	struct cas_provider_request *req;
	struct cas_provider_ignore *ignore;
	bool found = false;

	mutex_lock(&provider->requests_lock);
	hash_for_each_possible(provider->requests, req, node, id) {
		if (id == req->id) {
			found = true;
			break;
		}
	}
	if (likely(found)) {
		req->result = result;
		if (result == fetch_success) {
			req->size = size;
			req->mode = mode;

			switch (t) {
			case resp_blob:
				req->resp_type = cas_type_blob;
				break;
			case resp_obj:
				req->resp_type = cas_type_object;
				break;
			default:
				pr_err("unknown type 0x%02x when processing stat message",
				       req->type);
			}
		}

		wake_up(&req->wait);
	} else {
		hash_for_each_possible(provider->ignore_reqs, ignore, node,
				       id) {
			if (id == ignore->id) {
				hash_del(&ignore->node);
				kfree(ignore);
				found = true;
				break;
			}
		}
	}

	mutex_unlock(&provider->requests_lock);

	if (unlikely(!found))
		pr_warn("received a response with id=0x%08x with no requests\n",
			id);
}

static void cas_provider_process_status_msg(struct cas_provider *provider,
					    unsigned int id,
					    enum fetch_result result,
					    int error_code)
{
	struct cas_provider_request *req;
	struct cas_provider_ignore *ignore;
	bool found = false;

	mutex_lock(&provider->requests_lock);
	hash_for_each_possible(provider->requests, req, node, id) {
		if (id == req->id) {
			req->result = result;
			req->error_code = error_code;
			found = true;
			break;
		}
	}
	if (likely(found)) {
		wake_up(&req->wait);
	} else {
		hash_for_each_possible(provider->ignore_reqs, ignore, node,
				       id) {
			if (id == ignore->id) {
				hash_del(&ignore->node);
				kfree(ignore);
				found = true;
				break;
			}
		}
	}
	mutex_unlock(&provider->requests_lock);

	if (unlikely(!found))
		pr_warn("received a response with id=0x%08x with no requests\n",
			id);
}

static void cas_provider_process_msg(struct cas_provider *provider,
				     unsigned int id, enum fetch_result result)
{
	struct cas_provider_request *req;
	struct cas_provider_ignore *ignore;
	bool found = false;

	mutex_lock(&provider->requests_lock);
	hash_for_each_possible(provider->requests, req, node, id) {
		if (id == req->id) {
			found = true;
			break;
		}
	}
	if (likely(found)) {
		req->result = result;
		wake_up(&req->wait);
	} else {
		hash_for_each_possible(provider->ignore_reqs, ignore, node,
				       id) {
			if (id == ignore->id) {
				hash_del(&ignore->node);
				kfree(ignore);
				found = true;
				break;
			}
		}
	}

	mutex_unlock(&provider->requests_lock);

	if (unlikely(!found))
		pr_warn("received a response with id=0x%08x with no requests\n",
			id);
}

static int cas_provider_process_buf(struct cas_provider *provider,
				    const byte_t *buf, int n)
{
	int i = 0;
	bool enough_length = true;

	while (i + _MIN_PROVIDER_RESP_LENGTH <= n) {
		u8 msg_type = buf[i];

		switch (msg_type) {
		case msg_type_fetch_blob_resp:
		case msg_type_fetch_tree_resp:
		case msg_type_fetch_object_resp:
		case msg_type_fetch_ari_resp: {
			unsigned int id = get_unaligned_le32(buf + i + 1);
			enum fetch_result result = buf[i + 5];

			atomic64_inc(&provider->msg_counters[msg_type]);

			cas_provider_process_msg(provider, id, result);
			i += 6;
			break;
		}

		case msg_type_fetch_stat_resp: {
			unsigned int id;
			enum fetch_result result;
			uint64_t size;
			uint32_t mode;
			enum cas_resp_obj_type resp_type;

			enough_length = (i + _STAT_RESP_LENGTH <= n);
			if (!enough_length)
				break;

			id = get_unaligned_le32(buf + i + 1);
			result = buf[i + 5];
			size = get_unaligned_le64(buf + i + 6);
			mode = get_unaligned_le32(buf + i + 14);
			resp_type = buf[i + 18];

			atomic64_inc(&provider->msg_counters[msg_type]);

			cas_provider_process_stat_msg(provider, id, result,
						      size, mode, resp_type);
			i += 19;
			break;
		}
		case msg_type_check_status_resp: {
			unsigned int id;
			enum fetch_result result;
			int error_code;

			enough_length = (i + _STATUS_RESP_LENGTH <= n);
			if (!enough_length)
				break;

			id = get_unaligned_le32(buf + i + 1);
			result = buf[i + 5];
			error_code = (int)get_unaligned_le32(buf + i + 6);

			atomic64_inc(&provider->msg_counters[msg_type]);

			cas_provider_process_status_msg(provider, id, result,
							error_code);
			i += _STATUS_RESP_LENGTH;
			break;
		}
		default:
			pr_err("received unknown message type 0x%02x\n",
			       buf[i]);
			return -1;
		}

		if (!enough_length)
			break;
	}
	return i;
}

static int cas_provider_receiver(void *data)
{
	struct cas_provider *provider = (struct cas_provider *)data;
	int p = 0, q, n;
	struct kvec vec;
	struct socket *sock;
	struct msghdr msg = {
		.msg_flags = 0,
	};

	byte_t *buf;

	buf = kmalloc(1024, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	while (likely(!kthread_should_stop())) {
		mutex_lock(&provider->write_lock);
		if (unlikely(!provider->sock)) {
			if (test_bit(CAS_PROVIDER_UNMOUNTING,
				     &provider->flags)) {
				mutex_unlock(&provider->write_lock);
				break;
			}
			cas_provider_connect(provider);
		}
		sock = provider->sock;
		mutex_unlock(&provider->write_lock);
		if (!sock) {
			/* Throttle before retrying connect */
			msleep_interruptible(250);
			continue;
		}
		vec.iov_base = buf + p;
		vec.iov_len = 1024 - p;
		memset(&msg, 0, sizeof(msg));

		n = kernel_recvmsg(sock, &msg, &vec, 1, 1024 - p,
				   msg.msg_flags);
		if (n == -EAGAIN)
			continue;
		if (unlikely(n == -ERESTARTSYS))
			continue;
		if (unlikely(n <= 0)) {
			if (!READ_ONCE(provider->conn_err_logged)) {
				if (n == 0) {
					pr_err("cache-socket %s closed cleanly by peer (EOF)\n",
					       provider->socket_path);
					n = -ECONNRESET;
				} else {
					pr_err("failed to read from cache-socket %s: %pe\n",
					       provider->socket_path,
					       ERR_PTR(n));
				}
				WRITE_ONCE(provider->conn_err_logged, true);
			}
			mutex_lock(&provider->write_lock);
			cas_provider_close(provider, n, true);
			mutex_unlock(&provider->write_lock);
			p = 0;
			continue;
		}

		q = cas_provider_process_buf(provider, buf, n + p);
		if (q < 0) {
			mutex_lock(&provider->write_lock);
			cas_provider_close(provider, -EPROTO, true);
			mutex_unlock(&provider->write_lock);
			p = 0;
			continue;
		}
		p = n + p - q;
		memmove(buf, buf + q, p);
	}

	kfree(buf);
	return 0;
}

static void cas_provider_notification_worker(struct work_struct *work)
{
	struct cas_notify_work *nw =
		container_of(work, struct cas_notify_work, work);
	int err = 0;

	if (unlikely(test_bit(CAS_PROVIDER_UNMOUNTING, &nw->provider->flags)))
		goto out_free;

	switch (nw->action) {
	case CAS_NOTIFY_ACTION_USE:
		err = cas_provider_notify_use(nw->provider, &nw->hash);
		break;
	case CAS_NOTIFY_ACTION_NEW:
		err = cas_provider_new_object(nw->provider, &nw->hash,
					      nw->object_type);
		break;
	}

	if (unlikely(err && !test_bit(CAS_PROVIDER_UNMOUNTING,
				      &nw->provider->flags))) {
		if (err != -ERESTARTSYS && err != -EINTR)
			pr_err("Async %s notification failed for %20phN: %pe\n",
			       nw->action == CAS_NOTIFY_ACTION_NEW ?
				       "new object" :
				       "use",
			       &nw->hash, ERR_PTR(err));
	}

out_free:
	kmem_cache_free(nw->provider->notify_slab, nw);
}

static int debugfs_msg_stats_show(struct seq_file *m, void *v)
{
	struct cas_provider *provider = m->private;
	int i;

	seq_puts(m, "--- CASFS Provider Protocol Wire Audit ---\n");

	for (i = 0; i < ARRAY_SIZE(msg_stat_table); i++) {
		const struct msg_stat_entry *e = &msg_stat_table[i];

		seq_printf(m, "%-22s (%s) : %lld\n", e->name, e->hex,
			   (long long)atomic64_read(
				   &provider->msg_counters[e->type]));
	}

	seq_puts(m, "\n--- CASFS Provider Error Counters ---\n");
	seq_printf(m, "%-22s : %lld\n", "send_err_comm",
		   (long long)atomic64_read(&provider->stat_send_err_comm));
	seq_printf(m, "%-22s : %lld\n", "send_err_timeout",
		   (long long)atomic64_read(&provider->stat_send_err_timeout));
	seq_printf(m, "%-22s : %lld\n", "fetch_timeouts",
		   (long long)atomic64_read(&provider->stat_fetch_timeouts));
	seq_printf(m, "%-22s : %lld\n", "fetch_failures",
		   (long long)atomic64_read(&provider->stat_fetch_failures));

	return 0;
}

static int debugfs_msg_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, debugfs_msg_stats_show, inode->i_private);
}

static ssize_t debugfs_msg_stats_write(struct file *file,
				       const char __user *user_buf,
				       size_t count, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct cas_provider *provider = m->private;
	int i;

	for (i = 0; i < msg_type_max; i++)
		atomic64_set(&provider->msg_counters[i], 0);

	atomic64_set(&provider->stat_send_err_comm, 0);
	atomic64_set(&provider->stat_send_err_timeout, 0);
	atomic64_set(&provider->stat_fetch_timeouts, 0);
	atomic64_set(&provider->stat_fetch_failures, 0);

	return count;
}

static const struct file_operations fops_msg_stats = {
	.owner = THIS_MODULE,
	.open = debugfs_msg_stats_open,
	.read = seq_read,
	.write = debugfs_msg_stats_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static void provider_add_debugfs(struct cas_provider *provider,
				 struct dentry *parent)
{
	struct dentry *provider_dir;

	provider_dir = debugfs_create_dir("provider", parent);
	if (IS_ERR(provider_dir))
		return;

	debugfs_create_file("message_stats", 0644, provider_dir, provider,
			    &fops_msg_stats);
	debugfs_create_u64("socket_retries", 0444, provider_dir,
			   &provider->stat_socket_retries);
}

int cas_provider_init(struct cas_provider *provider, struct casfs_sysfs *sysfs,
		      struct cas_cache *cache, const char *socket_path,
		      int fetch_timeout, struct dentry *debugfs_parent,
		      int index)
{
	int i;
	int ret = 0;

	provider->sysfs = sysfs;
	provider->cache = cache;
	provider->task = NULL;
	provider->sock = NULL;
	provider->has_remote = socket_path != NULL;
	provider->fetch_timeout = fetch_timeout;
	provider->conn_err_logged = false;
	provider->flags = 0;
	atomic_set(&provider->last_id, 0);
	hash_init(provider->requests);
	hash_init(provider->ignore_reqs);
	mutex_init(&provider->write_lock);
	mutex_init(&provider->requests_lock);

	for (i = 0; i < msg_type_max; i++)
		atomic64_set(&provider->msg_counters[i], 0);

	provider->stat_socket_retries = 0;
	atomic64_set(&provider->stat_send_err_comm, 0);
	atomic64_set(&provider->stat_send_err_timeout, 0);
	atomic64_set(&provider->stat_fetch_timeouts, 0);
	atomic64_set(&provider->stat_fetch_failures, 0);

	/* Initialize upstream status (0 = healthy) */
	atomic_set(&provider->upstream_status, 0);

	/* Set structural unique identifier tracking name */
	snprintf(provider->wq_name, sizeof(provider->wq_name), "casfs_wq_%d",
		 index);
	snprintf(provider->slab_name, sizeof(provider->slab_name),
		 "casfs_notify_%d", index);

	provider->notify_slab = kmem_cache_create(
		provider->slab_name, sizeof(struct cas_notify_work), 0,
		SLAB_HWCACHE_ALIGN | SLAB_ACCOUNT, NULL);

	if (!provider->notify_slab) {
		pr_err("Failed to create provider slab cache %s\n",
		       provider->slab_name);
		return -ENOMEM;
	}

	provider->notify_wq = alloc_workqueue(
		provider->wq_name, WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_SYSFS, 1);
	if (!provider->notify_wq) {
		pr_err("Failed to create provider workqueue %s\n",
		       provider->wq_name);
		ret = -ENOMEM;
		goto out_free_kmem_cache;
	}

	if (debugfs_parent)
		provider_add_debugfs(provider, debugfs_parent);

	INIT_DELAYED_WORK(&provider->poll_work, cas_provider_poll_worker);

	if (!provider->has_remote)
		return 0;

	if (strscpy(provider->socket_path, socket_path,
		    sizeof(provider->socket_path)) == -E2BIG) {
		pr_err("ctrl: Socket path is too long!\n");
		ret = -EINVAL;
		goto out_free_workqueue;
	}
	provider->addr.sun_family = AF_UNIX;
	strscpy(provider->addr.sun_path, socket_path,
		sizeof(provider->addr.sun_path));

	queue_delayed_work(system_long_wq, &provider->poll_work,
			   CAS_PROVIDER_POLL_INTERVAL_HZ);

	ret = cas_provider_connect(provider);
	if (unlikely(ret)) {
		cancel_delayed_work_sync(&provider->poll_work);
		goto out_free_workqueue;
	}

	return 0;

out_free_workqueue:
	destroy_workqueue(provider->notify_wq);
	provider->notify_wq = NULL;
out_free_kmem_cache:
	kmem_cache_destroy(provider->notify_slab);
	provider->notify_slab = NULL;
	return ret;
}

int cas_provider_start(struct cas_provider *provider)
{
	struct task_struct *task;

	if (!provider->has_remote)
		return 0;

	task = kthread_create(cas_provider_receiver, provider,
			      "casfs_provider");

	if (IS_ERR(task))
		return PTR_ERR(task);

	get_task_struct(task);
	provider->task = task;
	wake_up_process(task);
	return 0;
}

void cas_provider_destroy(struct cas_provider *provider)
{
	struct cas_provider_ignore *ignore;
	struct task_struct *task;
	struct hlist_node *tmp;
	unsigned int bkt;

	// Stop any future requests
	set_bit(CAS_PROVIDER_UNMOUNTING, &provider->flags);

	if (provider->has_remote)
		cancel_delayed_work_sync(&provider->poll_work);

	if (provider->notify_wq) {
		destroy_workqueue(provider->notify_wq);
		provider->notify_wq = NULL;
	}

	kmem_cache_destroy(provider->notify_slab);
	provider->notify_slab = NULL;

	if (!provider->has_remote)
		return;

	mutex_lock(&provider->write_lock);
	if (provider->sock)
		kernel_sock_shutdown(provider->sock, SHUT_RDWR);
	mutex_unlock(&provider->write_lock);

	task = xchg(&provider->task, NULL);
	if (task) {
		kthread_stop(task);
		put_task_struct(task);
	}

	mutex_lock(&provider->write_lock);
	if (provider->sock)
		cas_provider_close(provider, 0, true);

	mutex_unlock(&provider->write_lock);

	mutex_lock(&provider->requests_lock);
	hash_for_each_safe(provider->ignore_reqs, bkt, tmp, ignore, node) {
		hash_del(&ignore->node);
		kfree(ignore);
	}
	mutex_unlock(&provider->requests_lock);
}

/* Send message to provider, attempt for 30 seconds to add the message
 * request the socket.
 *
 * Returns:
 *    0            : success
 *    -EIO         : error writing or framing message
 *    -ERESTARTSYS : received interrupt
 *    -ETIMEDOUT   : timed out trying to add to message queue
 */
static int cas_provider_send_message(struct cas_provider *provider, byte_t *buf,
				     size_t size)
{
	int err = 0;
	struct msghdr msg = {
		.msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL,
	};
	struct kvec iov = {
		.iov_base = buf,
		.iov_len = size,
	};

	if (!provider->has_remote)
		return -EIO;

	for (int i = 0; i < SEND_MESSAGE_TRIES; i++) {
		mutex_lock(&provider->write_lock);

		if (likely(provider->sock)) {
			if (unlikely(!sock_writeable(provider->sock->sk))) {
				err = -EAGAIN;
			} else {
				err = kernel_sendmsg(provider->sock, &msg, &iov,
						     1, size);
			}
		} else {
			err = -EAGAIN;
		}

		if (err == size) {
			if (buf[0] < msg_type_max)
				atomic64_inc(&provider->msg_counters[buf[0]]);
			mutex_unlock(&provider->write_lock);
			return 0;
		}

		if (err <= 0 && err != -EAGAIN && err != -EWOULDBLOCK &&
		    err != -ERESTARTSYS && err != -EINTR) {
			if (!READ_ONCE(provider->conn_err_logged)) {
				pr_err("failed to write to cache-socket %s: %pe\n",
				       provider->socket_path, ERR_PTR(err));
				WRITE_ONCE(provider->conn_err_logged, true);
			}
			if (provider->sock)
				kernel_sock_shutdown(provider->sock, SHUT_RDWR);

			mutex_unlock(&provider->write_lock);
			atomic64_inc(&provider->stat_send_err_comm);
			return -EIO;
		}

		if (unlikely(err > 0 && err < size)) {
			if (!READ_ONCE(provider->conn_err_logged)) {
				pr_err("partial write to cache-socket %s (%d/%zu bytes)\n",
				       provider->socket_path, err, size);
				WRITE_ONCE(provider->conn_err_logged, true);
			}
			if (provider->sock)
				kernel_sock_shutdown(provider->sock, SHUT_RDWR);

			mutex_unlock(&provider->write_lock);
			atomic64_inc(&provider->stat_send_err_comm);
			return -EIO;
		}

		mutex_unlock(&provider->write_lock);

		if (err == -ERESTARTSYS || err == -EINTR)
			return -ERESTARTSYS;

		msleep_interruptible(50);
		if (signal_pending(current))
			return -ERESTARTSYS;
	}

	if (!READ_ONCE(provider->conn_err_logged) &&
	    !test_bit(CAS_PROVIDER_UNMOUNTING, &provider->flags)) {
		pr_err("failed to write to cache-socket %s: timed out after %d retries\n",
		       provider->socket_path, SEND_MESSAGE_TRIES);
		WRITE_ONCE(provider->conn_err_logged, true);
	}

	atomic64_inc(&provider->stat_send_err_timeout);
	return -ETIMEDOUT;
}

void cas_provider_queue_new_object(struct cas_provider *provider,
				   const struct cas_hash *hash,
				   enum cas_req_obj_type type)
{
	struct cas_notify_work *nw;

	// Guard against notification during unmount
	if (unlikely(test_bit(CAS_PROVIDER_UNMOUNTING, &provider->flags)))
		return;

	/* Don't sleep or trigger kernel warnings if system memory is low */
	nw = kmem_cache_alloc(provider->notify_slab, GFP_NOWAIT | __GFP_NOWARN);
	if (unlikely(!nw)) {
		/* Fallback synchronously right here in user thread context */
		cas_provider_new_object(provider, hash, type);
		return;
	}

	INIT_WORK(&nw->work, cas_provider_notification_worker);
	nw->provider = provider;
	nw->hash = *hash;
	nw->object_type = type;
	nw->action = CAS_NOTIFY_ACTION_NEW;

	queue_work(provider->notify_wq, &nw->work);
}

void cas_provider_queue_notify_use(struct cas_provider *provider,
				   const struct cas_hash *hash)
{
	struct cas_notify_work *nw;

	if (unlikely(test_bit(CAS_PROVIDER_UNMOUNTING, &provider->flags)))
		return;

	nw = kmem_cache_alloc(provider->notify_slab, GFP_NOWAIT | __GFP_NOWARN);
	if (unlikely(!nw)) {
		cas_provider_notify_use(provider, hash);
		return;
	}

	INIT_WORK(&nw->work, cas_provider_notification_worker);
	nw->provider = provider;
	nw->hash = *hash;
	nw->action = CAS_NOTIFY_ACTION_USE;

	queue_work(provider->notify_wq, &nw->work);
}

int cas_provider_new_object(struct cas_provider *provider,
			    const struct cas_hash *hash,
			    enum cas_req_obj_type type)
{
	byte_t buf[21];

	if (!provider->has_remote)
		return 0;

	buf[0] = cas_type_to_new_object(type);
	memcpy(buf + 1, hash, sizeof(*hash));

	return cas_provider_error(
		cas_provider_send_message(provider, buf, sizeof(buf)));
}

int cas_provider_notify_use(struct cas_provider *provider,
			    const struct cas_hash *hash)
{
	byte_t buf[21];

	if (!provider->has_remote)
		return 0;

	buf[0] = msg_type_notify_use;
	memcpy(buf + 1, hash, sizeof(*hash));

	return cas_provider_error(
		cas_provider_send_message(provider, buf, sizeof(buf)));
}

static int cas_provider_send_ari_req(struct cas_provider *provider,
				     const struct cas_provider_request *req)
{
	byte_t *buf;
	size_t msg_len, ari_len;
	int err;

	if (!req->ari)
		return -EINVAL;

	ari_len = strnlen(req->ari, CAS_MAX_ARI_LEN + 1);
	if (ari_len == 0 || ari_len > CAS_MAX_ARI_LEN)
		return -EINVAL;

	msg_len = 7 + ari_len;
	buf = kmalloc(msg_len, GFP_KERNEL);
	if (unlikely(!buf))
		return -ENOMEM;

	buf[0] = msg_type_fetch_ari;
	put_unaligned_le32(req->id, buf + 1);
	put_unaligned_le16(ari_len, buf + 5);
	memcpy(buf + 7, req->ari, ari_len);

	err = cas_provider_send_message(provider, buf, msg_len);
	kfree(buf);
	return err;
}

static int cas_provider_send_req(struct cas_provider *provider,
				 const struct cas_provider_request *req)
{
	byte_t buf[25];

	if (req->type == msg_type_fetch_ari)
		return cas_provider_send_ari_req(provider, req);

	if (req->type == msg_type_check_status) {
		buf[0] = req->type;
		put_unaligned_le32(req->id, buf + 1);
		return cas_provider_send_message(provider, buf, 5);
	}

	buf[0] = req->type;
	put_unaligned_le32(req->id, buf + 1);
	memcpy(buf + 5, req->hash, sizeof(*req->hash));

	return cas_provider_send_message(provider, buf, sizeof(buf));
}

static void cas_provider_format_target(const struct cas_provider_request *req,
				       const struct cas_hash *hash, char *buf,
				       size_t buf_size)
{
	if (req->type == msg_type_fetch_ari && req->ari)
		snprintf(buf, buf_size, "%s", req->ari);
	else if (req->type == msg_type_check_status)
		snprintf(buf, buf_size, "check-status");
	else
		snprintf(buf, buf_size, "%20phN", hash);
}

static int cas_provider_handle_request(struct cas_provider *provider,
				       const struct cas_hash *hash,
				       struct cas_provider_request *req)
{
	int err;
	struct cas_provider_ignore *ignore;
	char target[128];
	unsigned long timeout_jiffies;

	if (!provider->has_remote)
		return -ENOENT;

	cas_provider_format_target(req, hash, target, sizeof(target));

	init_waitqueue_head(&req->wait);

	mutex_lock(&provider->requests_lock);
	hash_add(provider->requests, &req->node, req->id);
	mutex_unlock(&provider->requests_lock);

	err = cas_provider_send_req(provider, req);
	if (unlikely(err)) {
		if (req->type == msg_type_check_status)
			err = -ENETDOWN;
		else if (err != -ERESTARTSYS)
			pr_warn("failed to send request 0x%08x to fetch %s %s (%pe)\n",
				req->id, msg_type_to_string(req->type), target,
				ERR_PTR(err));

		mutex_lock(&provider->requests_lock);
		hash_del(&req->node);
		mutex_unlock(&provider->requests_lock);

		return err;
	}
	if (req->type == msg_type_check_status)
		timeout_jiffies =
			msecs_to_jiffies(CAS_PROVIDER_STATUS_TIMEOUT_MS);
	else
		timeout_jiffies = msecs_to_jiffies(provider->fetch_timeout);

	err = wait_event_interruptible_timeout(
		req->wait, req->result != fetch_wait, timeout_jiffies);

	mutex_lock(&provider->requests_lock);
	hash_del(&req->node);
	mutex_unlock(&provider->requests_lock);

	if (unlikely(err <= 0)) {
		/*
		 * In case of a timeout (err == 0) or an interrupt while waiting
		 * (`err == -ERESTARTSYS` which is the only allowed error), we
		 * don't have the result yet, therefor to avoid logging errors
		 * when the result arrives, we add the request-id to the ignore
		 * list.
		 */

		ignore = kmalloc(sizeof(*ignore), GFP_KERNEL);
		if (likely(ignore)) {
			mutex_lock(&provider->requests_lock);
			ignore->id = req->id;
			hash_add(provider->ignore_reqs, &ignore->node, req->id);
			mutex_unlock(&provider->requests_lock);
		}
		if (unlikely(err < 0))
			return err;
		req->result = fetch_timeout;
	}

	if (req->type == msg_type_check_status) {
		if (req->result != fetch_success)
			return -ENETDOWN;

		return -abs(req->error_code);
	}

	switch (req->result) {
	case fetch_success:
		return 0;
	case fetch_not_found:
		if (req->type != msg_type_fetch_stat)
			pr_info("server could not find %s #0x%08x %s\n",
				msg_type_to_string(req->type), req->id, target);
		return -ENOENT;
	case fetch_failed:
		atomic64_inc(&provider->stat_fetch_failures);
		pr_warn("fetch %s failed #0x%08x %s\n",
			msg_type_to_string(req->type), req->id, target);
		return -EIO;
	case fetch_timeout:
		atomic64_inc(&provider->stat_fetch_timeouts);
		pr_warn("fetch %s timed out for #0x%08x %s\n",
			msg_type_to_string(req->type), req->id, target);
		return -ETIMEDOUT;
	default:
		pr_err("unknown response result to fetch %s #0x%08x %s: 0x%02x\n",
		       msg_type_to_string(req->type), req->id, target,
		       req->result);
		return -EIO;
	}
}

static int cas_provider_wait_for_hash(struct cas_provider *provider,
				      const struct cas_hash *hash,
				      enum cas_req_obj_type type)
{
	struct cas_provider_request req = {
		.id = atomic_inc_return_acquire(&provider->last_id),
		.type = cas_type_to_fetch(type),
		.hash = hash,
		.result = fetch_wait,
	};

	return cas_provider_handle_request(provider, hash, &req);
}

static int cas_provider_wait_for_hash_git(struct cas_provider *provider,
					  const struct cas_hash *hash)
{
	struct cas_provider_request req = {
		.id = atomic_inc_return_acquire(&provider->last_id),
		.type = msg_type_fetch_git_object,
		.hash = hash,
		.result = fetch_wait,
	};

	return cas_provider_handle_request(provider, hash, &req);
}

static int cas_provider_wait_for_hash_stat(struct cas_provider *provider,
					   const struct cas_hash *hash,
					   struct cas_stat_info *stat)
{
	int err;
	struct cas_provider_request req = {
		.id = atomic_inc_return_acquire(&provider->last_id),
		.type = msg_type_fetch_stat,
		.hash = hash,
		.result = fetch_wait,
	};

	err = cas_provider_handle_request(provider, hash, &req);
	if (err != 0)
		return err;

	stat->size = req.size;
	stat->mode = req.mode;
	stat->type = req.resp_type;
	return err;
}

static int cas_provider_wait_for_ari(struct cas_provider *provider,
				     const char *ari)
{
	struct cas_provider_request req = {
		.id = atomic_inc_return_acquire(&provider->last_id),
		.type = msg_type_fetch_ari,
		.ari = ari,
		.result = fetch_wait,
	};
	size_t len;

	if (!ari)
		return -EINVAL;

	len = strnlen(ari, CAS_MAX_ARI_LEN + 1);
	if (len == 0 || len > CAS_MAX_ARI_LEN)
		return -EINVAL;

	return cas_provider_handle_request(provider, NULL, &req);
}

static int cas_provider_fetch_on_miss(struct cas_provider *provider,
				      const struct cas_hash *hash,
				      enum cas_req_obj_type type,
				      int initial_err)
{
	int err = try_provider(provider, initial_err);

	if (err)
		return err;

	return cas_provider_wait_for_hash(provider, hash, type);
}

static int cas_provider_fetch_ari_on_miss(struct cas_provider *provider,
					  const char *ari, int initial_err)
{
	int err = try_provider(provider, initial_err);

	if (err)
		return err;

	return cas_provider_wait_for_ari(provider, ari);
}

int cas_provider_get_blob_path(struct cas_provider *provider,
			       const struct cas_hash *hash, struct path *path)
{
	int err = cas_store_get_path(&provider->cache->blobs, hash, path);

	if (err) {
		err = cas_provider_fetch_on_miss(provider, hash, cas_type_blob,
						 err);
		if (likely(!err))
			err = cas_store_get_path(&provider->cache->blobs, hash,
						 path);
	}

	return cas_provider_error(err);
}

int cas_provider_get_blob_bytes(struct cas_provider *provider,
				const struct cas_hash *hash, void **read_buf,
				ssize_t *file_size)
{
	int err = cas_cache_get_blob_bytes(provider->cache, hash, read_buf,
					   file_size);
	if (err) {
		err = cas_provider_fetch_on_miss(provider, hash, cas_type_blob,
						 err);
		if (likely(!err))
			err = cas_cache_get_blob_bytes(provider->cache, hash,
						       read_buf, file_size);
	}

	return cas_provider_error(err);
}

int cas_provider_get_blob_size(struct cas_provider *provider,
			       const struct cas_hash *hash, ssize_t *size)
{
	int err = cas_cache_blob_size(provider->cache, hash, size);

	if (err) {
		err = cas_provider_fetch_on_miss(provider, hash, cas_type_blob,
						 err);
		if (likely(!err))
			err = cas_cache_blob_size(provider->cache, hash, size);
	}

	return cas_provider_error(err);
}

int cas_provider_get_object_bytes(struct cas_provider *provider,
				  const struct cas_hash *hash, void **read_buf,
				  ssize_t *file_size)
{
	int err = cas_cache_get_object_bytes(provider->cache, hash, read_buf,
					     file_size);
	if (err) {
		err = cas_provider_fetch_on_miss(provider, hash,
						 cas_type_object, err);
		if (likely(!err))
			err = cas_cache_get_object_bytes(provider->cache, hash,
							 read_buf, file_size);
	}
	return cas_provider_error(err);
}

int cas_provider_query_tree(struct cas_provider *provider,
			    const struct cas_hash *hash)
{
	int err = cas_cache_query_tree(provider->cache, hash);

	if (err == 0 || err == -ERESTARTSYS)
		return err;

	if (!provider->has_remote)
		return cas_provider_error(err);

	err = cas_provider_wait_for_hash(provider, hash, cas_type_tree);
	return cas_provider_error(err);
}

// The call will trigger the predict and prefetch of git objects in abfs.
// that might be something we don't expect to happened in the casfs.
int cas_provider_query_git(struct cas_provider *provider,
			   const struct cas_hash *hash)
{
	int err = cas_cache_query_object(provider->cache, hash);

	if (err == 0)
		return 0;
	if (unlikely(err == -ERESTARTSYS))
		return err;

	err = cas_cache_query_blob(provider->cache, hash);
	if (err == 0)
		return 0;

	if (unlikely(err == -ERESTARTSYS))
		return err;

	if (!provider->has_remote)
		return cas_provider_error(err);

	err = cas_provider_wait_for_hash_git(provider, hash);
	if (unlikely(err))
		return cas_provider_error(err);

	err = cas_cache_query_object(provider->cache, hash);
	if (err == 0)
		return 0;
	if (unlikely(err == -ERESTARTSYS))
		return err;

	err = cas_cache_query_blob(provider->cache, hash);
	return cas_provider_error(err);
}

int cas_provider_query(struct cas_provider *provider,
		       const struct cas_hash *hash, enum cas_req_obj_type *type)
{
	/* Order: local object, local blob, remote object, remote blob */
	int err = cas_cache_query_object(provider->cache, hash);

	if (err == 0) {
		*type = cas_type_object;
		return 0;
	}
	if (unlikely(err == -ERESTARTSYS))
		return err;

	err = cas_cache_query_blob(provider->cache, hash);
	if (err == 0) {
		*type = cas_type_blob;
		return 0;
	}

	if (unlikely(err == -ERESTARTSYS))
		return err;

	if (!provider->has_remote)
		return cas_provider_error(err);

	err = cas_provider_wait_for_hash(provider, hash, cas_type_object);
	if (unlikely(err))
		return cas_provider_error(err);

	err = cas_cache_query_object(provider->cache, hash);
	if (err == 0) {
		*type = cas_type_object;
		return 0;
	}
	if (unlikely(err == -ERESTARTSYS))
		return err;

	err = cas_cache_query_blob(provider->cache, hash);
	if (err == 0) {
		*type = cas_type_blob;
		return 0;
	}
	return cas_provider_error(err);
}

int cas_provider_query_stat(struct cas_provider *provider,
			    const struct cas_hash *hash,
			    struct cas_stat_info *stat)
{
	struct path path;
	enum cas_req_obj_type type;
	int err = cas_cache_query_object(provider->cache, hash);

	if (err == 0) {
		type = cas_type_object;
		err = cas_cache_object_path(provider->cache, hash, &path);
		if (err == 0)
			goto out_cached;
		return cas_provider_error(err);
	}
	if (unlikely(err == -ERESTARTSYS))
		return err;

	err = cas_cache_query_blob(provider->cache, hash);
	if (err == 0) {
		type = cas_type_blob;
		err = cas_cache_blob_path(provider->cache, hash, &path);
		if (err == 0)
			goto out_cached;
		return cas_provider_error(err);
	}

	if (unlikely(err == -ERESTARTSYS))
		return err;

	if (!provider->has_remote)
		return cas_provider_error(err);

	err = cas_provider_wait_for_hash_stat(provider, hash, stat);
	return cas_provider_error(err);

out_cached:
	stat->size = i_size_read(path.dentry->d_inode);
	stat->mode = path.dentry->d_inode->i_mode;
	stat->type = type;
	path_put(&path);
	return 0;
}

int cas_provider_path(struct cas_provider *provider, enum cas_req_obj_type type,
		      const struct cas_hash *hash, struct path *path)
{
	int err = -EINVAL;
	struct cas_store *store = (type == cas_type_blob) ?
					  &provider->cache->blobs :
					  &provider->cache->objects;

	if (type == cas_type_ari)
		return err;

	err = cas_store_get_path(store, hash, path);
	if (err) {
		err = cas_provider_fetch_on_miss(provider, hash, type, err);
		if (likely(!err))
			err = cas_store_get_path(store, hash, path);
	}

	return cas_provider_error(err);
}

// Find the blob by a ARI path, in this case the data is not part of our
// direct CAS stores (blobs,objects) and instead will reside in the
// ARI store, the data is addressed by the SHA1 of the ARI
int cas_provider_ari_path(struct cas_provider *provider, const char *ari,
			  struct path *path)
{
	int err = 0;
	struct cas_hash hash;
	size_t len;

	if (ari == NULL)
		return -ENOENT;

	len = strnlen(ari, CAS_MAX_ARI_LEN + 1);
	if (len == 0 || len > CAS_MAX_ARI_LEN)
		return -EINVAL;

	cas_hash_bytes(ari, len, &hash);
	err = cas_store_get_path(&provider->cache->aris, &hash, path);

	if (err) {
		err = cas_provider_fetch_ari_on_miss(provider, ari, err);
		if (likely(!err))
			err = cas_store_get_path(&provider->cache->aris, &hash,
						 path);
	}
	return cas_provider_error(err);
}

int cas_provider_tree_paths(struct cas_provider *provider,
			    const struct cas_hash *hash, struct path *path,
			    struct path *meta_path)
{
	int err = 0;

	err = cas_cache_object_path(provider->cache, hash, path);
	if (unlikely(err))
		return err;

	err = cas_cache_get_meta_tree_path(provider->cache, hash, meta_path);
	if (unlikely(err)) {
		path_put(path);
		return err;
	}
	return 0;
}

struct cas_provider_file *cas_provider_new_file(struct cas_provider *provider,
						enum cas_req_obj_type type)
{
	int rc = 0;
	struct cas_provider_file *file = NULL;
	struct cas_store *store = NULL;

	switch (type) {
	case cas_type_blob:
		store = &provider->cache->blobs;
		break;
	case cas_type_tree:
	case cas_type_object:
	default:
		store = &provider->cache->objects;
		break;
	}

	if (!store)
		return ERR_PTR(-EINVAL);

	file = kmalloc(sizeof(*file), GFP_KERNEL);
	if (!file)
		return ERR_PTR(-ENOMEM);

	file->provider = provider;
	file->type = type;
	file->store = store;
	file->filp = cas_store_create_scratch_file(file->store);
	if (!file->filp) {
		pr_err("failed to open temp file");
		rc = -EINVAL;
		goto out_err;
	}
	return file;

out_err:
	kfree(file);
	return ERR_PTR(rc);
}

int cas_provider_file_write(struct cas_provider_file *file, const char *buf,
			    size_t count, loff_t *pos)
{
	return kernel_write(file->filp, buf, count, pos);
}

int cas_provider_file_close(struct cas_provider_file *file,
			    const struct cas_hash *hash)
{
	// Close file, move file to correct location and update provider
	int rc = 0;
	struct dentry *dentry = dget(file_dentry(file->filp));

	rc = filp_close(file->filp, NULL);
	if (rc != 0)
		goto out_free;

	rc = cas_store_put_file(file->store, hash, dentry);
	if (unlikely(rc))
		cas_store_remove_scratch(file->store, dentry);

	cas_provider_queue_new_object(file->provider, hash, file->type);

out_free:
	dput(dentry);
	kfree(file);
	return rc;
}

int cas_provider_file_abort(struct cas_provider_file *file)
{
	int rc = 0;
	struct dentry *dentry = dget(file_dentry(file->filp));

	rc = filp_close(file->filp, NULL);
	if (rc != 0)
		goto out_free;

	cas_store_remove_scratch(file->store, dentry);

out_free:
	kfree(file);
	dput(dentry);
	return rc;
}

static int cas_provider_update_status(struct cas_provider *provider)
{
	struct cas_provider_request req = {
		.id = atomic_inc_return_acquire(&provider->last_id),
		.type = msg_type_check_status,
		.result = fetch_wait,
	};

	if (!provider->has_remote)
		return 0;

	return cas_provider_handle_request(provider, NULL, &req);
}

static void cas_provider_poll_worker(struct work_struct *work)
{
	struct cas_provider *provider =
		container_of(work, struct cas_provider, poll_work.work);
	int status;

	if (test_bit(CAS_PROVIDER_UNMOUNTING, &provider->flags))
		return;

	status = cas_provider_update_status(provider);
	if (status != -ENETDOWN)
		cas_provider_set_upstream_status(provider, status);

	if (!test_bit(CAS_PROVIDER_UNMOUNTING, &provider->flags))
		queue_delayed_work(system_long_wq, &provider->poll_work,
				   CAS_PROVIDER_POLL_INTERVAL_HZ);
}

int cas_provider_check(struct cas_provider *provider)
{
	if (!provider->has_remote)
		return 0;

	return atomic_read(&provider->upstream_status);
}
