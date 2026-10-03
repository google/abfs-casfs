# CASFS Cache Architecture and Transient Scratch Storage Specification

## Technical Design Document

---

## Subsystem Architecture Overview

CASFS segregates its underlying persistent structures from temporary file operations during transactional modifications.  Every super_block initialization creates an instance-specific cas_fs structure that aggregates a local cache instance.

       +-------------------------------------------------------+
       |                     cas_cache                         |
       +-------------------------------------------------------+
                    /              |               \
                   /               |                \
  +------------------+   +--------------------+   +-------------------+
  |   blobs store    |   |   objects store    |   |    scratch area   |
  |  (uncompressed)  |   | (zlib-compressed)  |   | (mount-isolated)  |
  +------------------+   +--------------------+   +-------------------+

The data structure orchestration maps as follows:

*   struct cas_cache: The top-level administrative context pinning credential states (struct cred), user namespaces, and the tracking paths for underlying cache targets.
*   struct cas_store blobs: Manages uncompressed, backing payload structures. Files are addressed cleanly by their cryptographic content hashes.
*   struct cas_store objects: Manages structured tree blocks, directory maps, metadata mappings, and structural tables (fully metadata-compressed via zlib deflate routines).
*   struct cas_scratch scratch: An isolated, per-mount dynamic canvas designed specifically to accept transient file handles and modification writes before they are formally frozen and committed to long-term immutable blocks.

---

## Core Cache Store Architecture (cas_store)

To prevent filesystem block saturation inside a singular structural folder, each cas_store splits paths into a multi-tiered hexadecimal prefix matrix configured by CAS_STORE_PREFIX_HEX_DIGITS.

| Configuration Attribute | Architectural Setting | Functional Detail |
| :--- | :--- | :--- |
| Prefix Split Dimension | 3 Hex Digits | Extracts the first 12 bits of the structural hash |
| Branch Directory Width | 4,096 Sub-folders | Statically generated from 000 to fff |
| Address Allocation Mapping | hash_hex + 3 | Truncates prefix values from child file listings to keep lookups performant |

### Backing File Operations

*   Read-Through Pathing (cas_store_read_all): Performs a direct, continuous stream extraction using kernel_read on underlying blocks. If flagged as an object branch, block extractions naturally flow into inline decompression mechanisms via casfs_inflate.
*   Atomic Linkage (cas_store_put_file): Accepts active descriptors from scratch transaction loops and maps them directly into designated content-hashed target fields using a non-replacing VFS namespace swap (vfs_rename with RENAME_NOREPLACE).

---

## Transactional Scratch Storage (cas_scratch)

The scratch workspace represents the absolute dynamic edge of the filesystem. No content-addressed blob modification directly impacts steady-state cache addresses until transactions finalize.

### Sharded Topology Mapping

An instance-isolated scratch space folder is generated at mount time under the format scratch/mnt_[IDA_ID]. To shield the top-level VFS namespace from locks and scaling pressure when multiple concurrent transactions occur, the workspace is partitioned into 256 independent shard folders (0 to 255) defined by CAS_STORE_SCRATCH_DIR_MASK:

scratch/
└── mnt_0/                  <-- Mount instance isolation boundary
    ├── 0/
    ├── 1/
    ...
    ├── 255/                <-- Shard buckets indexed via (inum & 0xFF)
    └── tmp_1               <-- Un-sharded transaction allocation

### Allocation & Uniqueness Controls

Temporary file handles use lock-free increment passes over a monotonic sequence counter (atomic64_t next_id). 

To completely rule out collision overlaps with numeric directory ranges, temporary file tags explicitly append alphabetic markers using a zero-allocation stack workspace:

    id = atomic64_inc_return(&scratch->next_id);
    snprintf(name, sizeof(name), "tmp_%llu", id);

### Self-Healing Crash Recovery

When a super_block mounts an existing workspace, any missing folders or previous structure instances trap -EEXIST safely to bypass error limits. If a crashed or uncleaned mount left files behind, the tracking engine references them using dentry_open accompanied by O_TRUNC. 

This setup naturally forces immediate truncations of zombie allocations down to 0 bytes on reuse, letting the filesystem self-heal without expensive initialization loops or explicit disk scans.

---

## Writable Blobs & File Modification Pipeline

The relationship between the VFS layer (cas_file_operations) and storage handles relies heavily on the internal abstraction wrapper struct cas_blob.

  VFS Structural Layer
          │
          ▼
   [struct cas_node]  ──(owns)──►  [struct cas_blob]
          │                                │
          ▼                                ▼
   POSIX Namespace Management       State Machine & IO Engine
   (VFS Inode Metadata, Locks)      (File Handles, Paths, Modes)

The behavioral cycle of an active file modification involves four distinct execution phases:

### Phase 1: Lifecycle Initialization
When a target file is accessed, the operational layout defaults to cas_open_mode_closed. When reading or writing occurs, initialization routines transition the tracking mode to cas_open_mode_loaded, using cas_provider_path to verify if a valid immutable target copy exists locally.

### Phase 2: Copy-on-Write (CoW) Detachment
When an access call contains modify, truncate, or write-intent flags, cas_blob_ensure_write activates a structural CoW sequence:
1. If the node points to an active immutable read block, the asset size is captured using i_size_read.
2. A completely blank, un-sharded temporary file structure is provisioned via cas_store_create_scratch_file_inum.
3. If an existing payload is present, it is copied directly to the new scratch block via vfs_copy_file_range.
4. Read descriptors close, and the tracking path swaps context entirely over to the dynamic scratch descriptor, changing the operational mode to cas_open_mode_rw and setting the node state to Dirty.

### Phase 3: In-Flight Modifications
All subsequent VFS modifications (write_iter, fallocate, truncate) pass through safely to the backing scratch file. The size parameters of the file reflect dynamic changes in line with raw scratch modifications, cleanly exposing updates back to the parent context.

### Phase 4: Snapshot, Freeze, & Commit
When data changes commit (via cas_file_snapshot), the transaction closes securely:

[Dynamic Scratch File] ──► Read & Generate Cryptographic Hash ──► kfree(file)
                                                                       │
[Immutable Store Folder] ◄── vfs_rename() ◄── [Retained Dentry Pointer]┘

1. The file is parsed to generate a final, immutable cryptographic verification key.
2. Active read/write blocks close, but the parent context retains an explicit reference to the underlying dentry.
3. The temporary asset maps safely into its immutable destination path inside the cas_store folder structure via cas_store_put_file.
4. If a separate mount instance created the same file hash first, the duplicate transaction resolves automatically. The system catches -EEXIST, discards the redundant file chunk with cas_scratch_remove, and uses the existing backing file.
5. The local file context drops its dirty status flags and resets safely back to cas_open_mode_closed.

---

## Directory Recalculation & Snapshot Pipeline

Directory changes use an asymmetrical tracking approach. Modifications to directories do not pass through a CoW tracking pipeline. Instead, they leverage a fast, volatile target layout that recalculates lazily on freeze calls.

Volatile Modification (mkdir, create, symlink, unlink)
  │
  ▼
Update volatile, in-memory RB-tree indexing (node->entries)
  │
  ▼
Bubble up change flags by increments to parent trees via cas_make_dirty()
  │
  ▼
[Lazy Snapshot Sync] ──► Flush state, balance trees, serialize to objects store

1. Volatile Mutation Path: Operations like mkdir, create, or symlink bypass the scratch file system. They update the directory's volatile, in-memory Red-Black tree layer (node->entries) directly.
2. Bubble-Up Dirtiness Notification: Once changed, cas_make_dirty() triggers an incremental modification loop that travels upward, raising the dirty flags on all ancestors until it hits the file system root.
3. Lazy Snapshot Consolidation: When sync loops call cas_dir_snapshot, a kernel worker thread pool (casfs.hasher) uses a lock-free approach to process the directory tree. The worker pool isolates change branches, processes pending hashes, and writes newly balanced structure definitions cleanly back into the compressed objects store.
