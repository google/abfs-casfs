# CASFS Provider Subsystem & Socket Lifecycle Architecture
The cas_provider subsystem manages bidirectional communication between the CASFS in-kernel filesystem module and the user-space cache daemon (e.g., abfs / casfs-daemon) over a local Unix domain stream socket (AF_UNIX, SOCK_STREAM). It fetches remote blobs, directory trees, ARI references, and metadata on demand, while streaming object creation and access notifications asynchronously.

## High-Level Architecture

      +--------------------------------------------------------+
      |               VFS Inode / File Operations              |
      +--------------------------------------------------------+
              |                                    |
      Synchronous RPCs                     Async Notifications
     (blob/tree/ari miss)                 (new object, notify use)
              |                                    |
              v                                    v
   +--------------------+               +-----------------------+
   | cas_provider_      |               |  cas_notify_work      |
   | handle_request()   |               |  (dedicated slab)     |
   +--------------------+               +-----------------------+
              |                                    |
              | Enqueue req->id                    | Workqueue
              v                                    v
     [requests hashtable]               cas_provider_send_message()
              |                                    |
              +-----------------+------------------+
                                |
                                v
                   [Unix Domain Socket: SOCK_STREAM]
                                |
                                v
               +----------------------------------+
               |   cas_provider_receiver kthread  |
               |       (Single Socket Owner)      |
               +----------------------------------+
                                |
                        Demux Message Type
                                |
        +-----------------------+-----------------------+
        |                                               |
  fetch_*_resp                                    check_status_resp
        |                                               |
  Lookup req->id                                  Wake health checker /
  in requests hash                                Update upstream_status
  & wake up waiter

## Concurrency & Synchronization Model
The provider uses distinct locks to prevent cross-subsystem deadlocks and avoid latency spikes on VFS paths:

Lock Name |	Data Protected | Rules & Invariants
----------| ---------------|-------------------
write_lock (struct mutex) |	provider->sock, socket connection state	| Held during connect(), sendmsg(), and socket shutdown. Never held across blocking sleeps or long waits.
requests_lock (struct mutex) |	provider->requests, provider->ignore_reqs	| Held only during fast hashtable insertions, deletions, lookups, and iteration.
atomic_t upstream_status  | Upstream health status (0 = healthy, negative = error)	| Read locklessly by VFS entry checks (cas_fs_check()). Updated via atomic_xchg().

## Strict Socket Lifecycle & Ownership Rules
To prevent use-after-free (UAF) crashes and NULL pointer dereferences in kernel network callbacks, CASFS enforces a Single Destruction Owner rule for provider->sock.

Rule 1: Senders Never Release the Socket
* VFS worker threads invoking cas_provider_send_message() acquire provider->write_lock only to transmit frames via kernel_sendmsg().

* If a transmission encounters a fatal error (e.g., -EPIPE, -ECONNRESET, corrupted framing), the sender must never call cas_provider_close() or sock_release().

* The sender calls kernel_sock_shutdown(provider->sock, SHUT_RDWR) while holding write_lock, logs the error once, and returns -EIO.

* Calling kernel_sock_shutdown() breaks any blocking read in the receiver thread without freeing the underlying struct socket out from under it.

Rule 2: The Receiver Thread Owns Normal Teardown & Reconnect
* The dedicated kernel thread cas_provider_receiver is the only runtime context permitted to call cas_provider_close() during normal filesystem operation.

* The receiver samples provider->sock under write_lock. If sock is NULL, it initiates cas_provider_connect().

* When kernel_recvmsg() unblocks with <= 0 (clean EOF or peer connection reset), the receiver acquires write_lock and invokes cas_provider_close(provider, err).

* Because the receiver is outside kernel_recvmsg() when calling cas_provider_close(), no thread is reading the socket when it is released.

Rule 3: Unmount Order Guarantees Receiver Termination First
During filesystem teardown in cas_provider_destroy():

1. CAS_PROVIDER_UNMOUNTING bit is set in provider->flags to block new requests and reconnect attempts.

2. kernel_sock_shutdown(sock, SHUT_RDWR) is called under write_lock to immediately unblock kernel_recvmsg() in the receiver thread.

3. kthread_stop(provider->task) is invoked, which blocks until cas_provider_receiver exits completely.

4. Only after the receiver thread is dead does cas_provider_destroy() acquire write_lock and invoke cas_provider_close() to release the socket.


## In-Flight Request Tracking & Disconnect Aborts
Synchronous fetches (blobs, trees, metadata) allocate a monotonic 32-bit request ID (provider->last_id) and register a struct cas_provider_request in provider->requests before writing to the wire:

* Wait Loop: The calling thread waits on req->wait using wait_event_interruptible_timeout().

* Clean Response: When cas_provider_receiver decodes a matching response ID, it assigns the result, removes the request from the hashtable under requests_lock, and wakes the waiter.

* Disconnect Fail-Fast: When the connection drops, cas_provider_close() calls cas_provider_abort_all_requests(). All in-flight requests transition immediately to fetch_failed and wake up. This prevents user threads from hanging for the full 120-second timeout on socket resets.

* Late Responses & Ignore Table: If a request times out or is interrupted by a signal (-ERESTARTSYS), its ID is inserted into provider->ignore_reqs. When late packets arrive, the receiver silently purges them from the ignore table without logging spurious warnings.

## Asynchronous Notification Pipeline
* Object updates (cas_provider_queue_new_object) and access markings (cas_provider_queue_notify_use) must never block critical VFS paths:

* Notifications allocate a struct cas_notify_work from a dedicated slab cache (provider->notify_slab) using GFP_NOWAIT | __GFP_NOWARN.

* Work items are dispatched to an unbound, single-threaded workqueue (provider->notify_wq).

* During unmount (CAS_PROVIDER_UNMOUNTING), all new and pending notifications are dropped to ensure clean workqueue draining.

## Wire Framing Conventions
* All frames exchanged across the domain socket must strictly adhere to little-endian alignment:

* Message Type: 1 byte (enum message_type).

* Request ID: 4 bytes, serialized using put_unaligned_le32() / get_unaligned_le32().

* Payload Hashes: 20 bytes binary hash (CASFS_HASH_LEN), copied via memcpy to prevent unaligned access faults across CPU architectures.
