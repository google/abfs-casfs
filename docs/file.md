# casfs Architectural Documentation: Inode Data Blob & Virtual Operations

This document covers the architectural layout of `casfs` files, focusing on how standard user-space file descriptors map down through the Virtual File System (VFS), our custom tracking components (`file.c` and `blob.c`), and the core synchronization locking strategies required to prevent data races, deadlocks, and kernel panics.

---

## 1. Architectural Map

When a user-space process interacts with a file descriptor on `casfs`, it passes through a tiered mapping layout. Rather than tracking raw file handles deep inside our internal data structures, `casfs` uses an intentional, decoupled overlay proxy strategy:

+-------------------------------------------------------+
|                     User Space                        |
|            Standard File Descriptor (fd)              |
+---------------------------+---------------------------+
                            |
                            v
+-------------------------------------------------------+
|                 Linux Kernel VFS                      |
|            struct file (Frontend Descriptor)          |
+---------------------------+---------------------------+
                            |
                            v  [file->private_data]
+-------------------------------------------------------+
|                  casfs file.c                         |
|      Maintains dynamic proxy via cas_file_private     |
|      Tracks layout state via locks / generation       |
+---------------------------+---------------------------+
                            |
                            v  [priv->real_file]
+-------------------------------------------------------+
|                  casfs blob.c                         |
|      Manages residency state, generation tracking,    |
|      and mutable scratch/immutable blob transitions   |
+---------------------------+---------------------------+
                            |
                            v  [node->blob.path]
+-------------------------------------------------------+
|               Host Native File System                 |
|          struct file / struct path (Backend)          |
+-------------------------------------------------------+


### Component Breakdown
* **User Space File:** The traditional integer handle manipulated via standard system calls (`read`, `write`, `mmap`).
* **VFS Layer (`struct file` Frontend):** The virtual tracking structure created by the Linux kernel when `casfs` intercepts an open transaction.
* **`cas_file_private` Wrapper:** Allocated per open descriptor by `file.c`. It acts as the anchor point for our real proxy files and isolates dynamic runtime promotions cleanly without cross-thread or global descriptor interference. It maintains a copy of the blob generation counter to trigger on-demand synchronization misses.
* **Residency Blob Engine (`blob.c`):** Tracks physical data location on local storage media. It manages state transitions between unmapped cache entries and active read/write scratch areas, acting as the ground-truth owner of the asset's structural generation.

---

## 2. Overlay Paradigm & Key Deviations

Architecturally, `casfs` functions as a specialized, stacked overlay file system similar to Linux's `overlayfs`. It intercepts frontend VFS entry points and transparently proxies execution down to backing descriptors hosted on an underlying native storage layer (e.g., `ext4`, `xfs`). 

However, `casfs` diverges fundamentally from standard overlay models in three key architectural exceptions:

### A. True Content-Addressable Copy-on-Write (CoW)
Standard `overlayfs` duplicates a file from a lower directory to an upper directory based on raw path names whenever a write flag is checked. In contrast, `casfs` evaluates files as **immutable hashes**. When a write operation cuts through a file, it clones the backing asset into a decoupled, sharded scratch pool managed entirely via transient tracking references. It only receives a permanent layout path once a transaction commits, recalculating the cryptographic hash of the newly written contents.

### B. Absolute Lazy Loading
Where stacked file systems often map metadata and paths early during lookup hooks, `casfs` defers lower-layer path resolution entirely. Backing files are not located, resolved, or pinned until a specific descriptor actively issues a read or write operation, keeping the system's runtime VFS memory footprint highly lean.

### C. Transactional Snapshotting
Modifications in `casfs` are isolated inside scratch spaces. When a file descriptor is closed, synced, or snapshot, the modified scratch state transitions atomically into the global, immutable content-addressable storage tier. This turns a standard file stream into an isolated transactional ledger item.

---

## 3. Inode Data Residency State Machine

To enforce data consistency across parallel transactions, an inode's background storage lifecycle is driven strictly by an analytical state machine inside `enum cas_cache_state`:

   [ CAS_CACHE_UNLOADED ]
              │
              │ cas_blob_load()
              ▼
   [ CAS_CACHE_RESIDENT ]
              │
              ├──► cas_blob_ensure_read() ──► [ CAS_CACHE_READ_ACTIVE ] ──┐
              │                                             │              │
              │              ┌──────────────────────────────┘              │
              │              │ (Mid-flight Write Upgrade)                  │ (File Closed /
              ▼              ▼                                             │  Cache Sync)
   [ cas_blob_ensure_write() ]                                             │
              │                                                            │
              ▼                                                            │
   [ CAS_CACHE_WRITE_ACTIVE ]                                              │
              │                                                            │
              ▼ (Snapshot Commit & Reset)                                  │
   [ cas_blob_snapshot() ]                                                 │
              │                                                            │
              └───────────────┬────────────────────────────────────────────┘
                              │
                              ▼
                   [ CAS_CACHE_UNLOADED ]

### State Definition Guarantees

* **`CAS_CACHE_UNLOADED`:** Local path maps are unlinked or unmapped. The file's contents reside safely in the persistent content-addressable provider database. Any subsequent access forces an on-demand retrieval layout sequence.
* **`CAS_CACHE_RESIDENT`:** The content's localized path layout has been successfully discovered or downloaded. A lower dentry hook is securely pinned to the file node, and `node->is_loaded` is globally marked true.
* **`CAS_CACHE_READ_ACTIVE`:** The file descriptor is actively optimized to stream read operations directly from the localized, read-only backend repository path via `cas_blob_ensure_read()`. When the file handle is closed, if it remains clean, it returns directly to the `UNLOADED` state during cache synchronization loops without ever triggering a snapshot.
* **`CAS_CACHE_WRITE_ACTIVE`:** The file has been promoted into a local writable scratch space via `cas_blob_ensure_write()`. This state is reached either directly from the initial `RESIDENT` layout or via a mid-flight write promotion that intercepts an active `READ_ACTIVE` context and forces an execution upgrade. It must pass through `cas_blob_snapshot()` to commit dirty blocks before returning to the `UNLOADED` state.

---

## 4. Concurrency, Locking, & Generational Invalidation

Because multiple execution threads can simultaneously read, write, truncate, or snapshot an identical data block, `casfs` utilizes a simplified, high-performance serialization strategy relying on native VFS-level locks and generation-based cache validation.

### A. Lock Hierarchy

Operational execution blocks must *always* acquire locks descending down the structural tree:

1. `inode_lock(upper_frontend_inode)` (VFS Mutex)
2. `cas_lock_node(upper_frontend_inode)` (casfs Custom Node Lock)
3. `inode_lock(lower_backend_inode)` (Lower Storage Target VFS Mutex)



### B. Generational Cache Validation under Native Serialization

To guarantee that file descriptors never access stale data or mismatched paths across parallel transactions, `casfs` combines atomic generations with standard VFS lock invariants:

* **The Generation Counter:** The `struct cas_blob` maintains an atomic generation counter (`atomic_t generation`), which increments globally whenever the backing layout changes (e.g., write promotions, snap commits, or truncation resets).
* **`_locked` Suffix Convention:** To prevent silent regression bugs, all helper functions that manipulate underlying state machine variables feature the `_locked` suffix (e.g., `cas_ensure_real_file_locked`, `cas_file_ensure_read_locked`).
---

## 5. Core Operational Isolation & Immutability Rules

### A. Immutability Hardening via `notify_change`
To guarantee that files are permanently isolated from out-of-band modifications or delayed kernel writeback routines, the backend cache engine enforces write-protection directly on the host storage tier. 

Before a temporary scratch dentry is renamed, linked, or migrated into its final content-addressable storage home (inside both `cas_store_put_file` for data blocks and `cas_store_put_meta_file` for tree metadata structures), it must have its write bits explicitly stripped:

```c
inode_lock(inode);
newattrs.ia_mode = inode->i_mode & ~S_IWUGO; /* Strip user, group, and world write privileges */
newattrs.ia_valid = ATTR_MODE;
err = casfs_notify_change(cas_store_ns(store), dentry, &newattrs);
inode_unlock(inode);

```


This structural lock-down forces the file on the underlying disk partition to become natively read-only (0444 or 0555), completely neutralizing lingering FMODE_WRITE kernel leaks or un-synchronized block transfers from triggering illegal background modifications.

### B. Isolation During Write Iterations (`cas_write_iter`)
To prevent lock contention and system-wide stalling during slow backing disk writes, `casfs` isolates the state machine promotion from the actual data payload transfer:

```c
/* Phase 1: Hold node lock strictly to check and promote the proxy file state */
cas_lock_node(inode);
ret = cas_file_ensure_write_locked(file, node, priv);
cas_unlock_node(inode); /* Released immediately to prevent I/O blocking stalls */

if (unlikely(ret))
	return ret;

/* Phase 2: Perform standard blocking write pass outside driver locking boundaries */
real_inode = file_inode(priv->real_file);
file_start_write(priv->real_file);
ret = cas_compat_write_iter(file, priv->real_file, iter, iocb);
file_end_write(priv->real_file);

```

### C. Safe Handle Teardown & Reference Counting
When cas_ensure_real_file_locked detects a layout generation change, it releases the old priv->real_file descriptor by calling filp_close(). Because all transitions are protected by cas_lock_node(inode), racing threads can never obtain a stale file pointer while it is being closed. Active I/O operations are pinned in memory using standard VFS internal reference counting (f_count), completely eliminating the need for custom, complex tracking mechanics in our private files.

### D. Deadlock Elimination During Snapshots (cas_file_snapshot)
Executing an unlink operation inside the lower scratch space (cas_store_remove_scratch) requires taking native VFS level locks on underlying host filesystem directories. To prevent cross-inode lock inversions against incoming path lookups, the data commit pipeline completely isolates structural calculation from layout clearing:

```c
/* Phase 1: Lock structural mutations and verify clean pages */
inode_lock(inode);
cas_lock_node(inode);
err = cas_blob_snapshot(&node->blob, ...); // Generates hash, commits object to database
cas_unlock_node(inode);
inode_unlock(inode);

/* Phase 2: Complete physical layout teardown entirely outside upper VFS execution bounds */
cas_lock_node(inode);
cas_blob_reset(&node->blob); // Safe from directory layout lock vectors
cas_unlock_node(inode);

```

## 6. Scratch Allocation Layout & Permission Lifecycles

To prevent file-system-level bottlenecks and guarantee strict permission consistency regardless of local system configurations, `casfs` isolates mutable data within a sharded scratch space before locking it down into permanent storage.

### A. Inode-Based Shard Routing
When a file is opened for writing or a modification pass is triggered, `casfs` allocates a temporary backing file via `cas_scratch_create_inum()`. 

To prevent thousands of concurrent execution threads from hitting a single directory lock boundary—which causes catastrophic lock contention on host filesystems like `ext4`—the scratch layer shards allocations across bucket directories using a bitmask calculation:

              [ Upper Inode Number (inum) ]
                            │
                            ▼
     [ bucket = inum & CAS_STORE_SCRATCH_DIR_MASK ]
                            │
                            ▼
       [ Path: /scratch/shard_dir/<bucket>/<inum> ]

This ensures that lookups, writes, and unlinks are distributed uniformly across multiple subdirectories, isolating VFS directory mutexes.

### B. Overriding the Host `umask`
When regular files or metadata structures are initially instantiated on the host layer using `vfs_create()` or `vfs_mkdir()`, the Linux kernel automatically subjects the requested mode bits (e.g., `0644` or `0755`) to the invoking process's `umask` string. If a user process runs under a restrictive mask like `0077`, the file would be stripped of group and world privileges, breaking access for background daemons or multi-tenant system services.

To ensure consistency, `casfs` implements an explicit **Post-Creation Fixup Pass**:
1. The file is created via standard VFS paths.
2. The engine immediately locks the newly created target inode.
3. It forces an absolute mode reset using `ATTR_FORCE` combined with `casfs_notify_change`.

This bypasses the active `umask` retroactively, ensuring that all temporary files settle reliably at a baseline of `0644` (for files) or `0755` (for shard directories).

### C. The Permanent CAS Immutability Transition
When a file handle is closed or a transaction commits, the file must migrate from its volatile, sharded scratch home into the global, content-addressed database (`blobs/` or `objects/`). During this phase, permissions shift from **mutable working buffers** to **un-alterable historical ledgers**:

| Phase | Path Tier | Applied Mode | VFS Operation | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| **1. Allocation** | `/scratch/...` | `0644` (Regular)<br>`0755` (Dir) | `vfs_create` + `ATTR_FORCE` | Allows active write operations and standard payload ingestion loops. |
| **2. Hardening** | `/scratch/...` | `mode & ~S_IWUGO` | `casfs_notify_change` | Strips all write permissions (`0444` / `0555`), freezing the backing blocks. |
| **3. Commitment** | `/[blobs\|objects]/` | `0444` / `0555` | `vfs_rename` | Moves the write-protected asset atomically into its deterministic hash home. |

By executing the permission drop via `notify_change` **before** the final `vfs_rename` takes place, `casfs` ensures that no file ever exists within the permanent content-addressed bucket directories with write privileges active, making it safe from un-synchronized kernel page cache writebacks or accidental parallel mutations.

7. Kernel Compatibility Layer
To bridge upstream Linux features with enterprise long-term support (LTS) kernel frameworks, casfs isolates virtual file interaction methods inside an internal compatibility interface (compat_file.h):

Kernels >= 6.6.0: The proxy architecture maps operations directly into native kernel tracking vectors via backing_file_open, backing_file_read_iter, backing_file_write_iter, and backing_file_mmap.

Legacy Kernels: The wrapper interface falls back to manual descriptor mapping and handling Virtual Memory Area (vma->vm_file) assignments manually.

Operational Flag Synthesis
Operational modifiers configured on the frontend descriptor (such as O_NONBLOCK and O_APPEND) must be explicitly propagated into the flags parameter passed to cas_backing_file_open.

While user-space operations are largely insulated by VFS iocb->ki_flags parsing, initializing the lower descriptor with proper flag fields is required to prevent write position regressions (offset zeroes) during direct kernel-initiated write tasks (e.g., layered VFS iterations, monitoring utilities, or stacked security modules) operating directly against the underlying proxy file reference.


## 8. Runtime System Resource Overhead Analysis

Because `casfs` operates as a stacked, content-addressed proxy layer, it exhibits unique memory and processing signatures distinct from traditional single-tier block file systems. Clients should expect and plan for the following overhead constraints.

### A. Memory Consumption & Double-Caching (Dentry/Inode Bloat)
For every active file tracked by `casfs`, the Linux kernel maintains a duplicate tracking structure within the virtual file system layer:
* **The Tax:** One frontend `struct inode`/`struct dentry` wrapper allocated for the `casfs` mount, and **one additional** backend `struct inode`/`struct dentry` allocated on the native host storage layer (e.g., `ext4` or `xfs`).
* **Sizing Guideline:** Double the typical memory allocation required for the VFS Slab Caches (`dentry` and `inode_cache`). On heavy file-count workloads (such as giant Git monorepos), memory consumption scales strictly with the number of concurrently tracked files rather than data volume size.

### B. Serialization Validation Overhead
* **The Read/Write Validation:** Every file operation validates its proxy layout by checking the generation count under the protection of `cas_lock_node`. Because this is a simple integer comparison under highly optimized mutex structures, it runs at maximum kernel speed.
* **The Mutation Invalidation Tax:** When a snapshot or write-promotion occurs, the generation counter increments. Every active file handle tracking that file will suffer a controlled cache re-validation. The serialized thread must call `filp_close()` on the old backing descriptor and execute a fresh path lookup to resolve the newly committed file. Systems experiencing heavy background snapshot churn will see increased CPU cycles allocated to VFS path-lookup loops.

### C. Sharded Scratch Disk Alignment & Metadata Amplification
The sharded storage topology defined in `/scratch/...` trades inode metadata overhead for structural lock parallelism:
* **Directory IOPS:** Creating, fixing up permissions (bypassing the host `umask`), and renaming files during snapshot transitions triggers elevated metadata write operations (IOPS) on the underlying physical drive. 
* **Storage Sizing:** Ensure the backing native file system is formatted with a generous inode allocation layout (e.g., maximizing `mkfs.xfs` or `mkfs.ext4` inode ratios). Running completely out of host filesystem inodes while physical block space remains unspent is a common failure vector if thousands of short-lived transient scratch files are generated rapidly.

### D. 1:1 Backing Descriptor Amplification (Global File Table Pressure)
When a user-space client opens a file descriptor and issues a read or write operation, `casfs` provisions a dedicated internal proxy tracking structure (`struct cas_file_private`) per open transaction.
* **The Tax:** For every active file descriptor held open by a client application on the frontend, `casfs` allocates an additional backend `struct file` handle via `backing_file_open()` pointing to the underlying host filesystem storage target. 
* **Process vs. Global Limits:** These backend proxy descriptors do not occupy slots within the client process's local user-space integer table (meaning they do not count against the process's local `ulimit -n` cap). However, they **do** consume entries in the Linux kernel’s global system file table (`/proc/sys/fs/file-nr`).
* **Sizing Guideline:** Systems hosting applications with heavy file concurrency must expect file table consumption to double. To prevent system-wide descriptor exhaustion errors, the kernel's global allocation bounds should be explicitly scaled up in production profiles via `sysctl`:
  ```bash
  sysctl -w fs.file-max=2097152
  ```
