# PES-VCS — Version Control System 

---

## Table of Contents

1. [Project Overview](#project-overview)
2. [Phase 1 — Object Storage](#phase-1--object-storage)
3. [Phase 2 — Tree Objects](#phase-2--tree-objects)
4. [Phase 3 — Staging Area (Index)](#phase-3--staging-area-index)
5. [Phase 4 — Commits and History](#phase-4--commits-and-history)
6. [Phase 5 — Branching and Checkout (Analysis)](#phase-5--branching-and-checkout-analysis)
7. [Phase 6 — Garbage Collection (Analysis)](#phase-6--garbage-collection-analysis)
8. [Integration Test](#integration-test)
9. [Implementation Notes](#implementation-notes)

---

## Project Overview

PES-VCS is a Git-inspired local version control system built from scratch in C. It implements content-addressable object storage, a text-based staging area, recursive tree construction, and a linked-list commit history — all backed by the `.pes/` directory structure.

**Commands implemented:**

| Command | Description |
|---|---|
| `pes init` | Create `.pes/` repository structure |
| `pes add <file>...` | Stage files (hash blob + update index) |
| `pes status` | Show staged / modified / untracked files |
| `pes commit -m <msg>` | Snapshot staged files as a commit |
| `pes log` | Walk and display commit history |

**Files implemented** (all others were provided and left unmodified):

| File | Functions Written |
|---|---|
| `object.c` | `object_write`, `object_read` |
| `tree.c` | `tree_from_index` |
| `index.c` | `index_load`, `index_save`, `index_add` |
| `commit.c` | `commit_create` |

---

## Phase 1 — Object Storage

### Implementation

**`object_write`** stores any piece of data in the content-addressable object store:

1. Prepends a type header `"blob 16\0"` / `"tree 42\0"` / `"commit 244\0"` to the raw data.
2. Computes SHA-256 of the full object (header + data) using OpenSSL's EVP API.
3. Checks for deduplication — if the hash already exists on disk, returns immediately.
4. Creates the shard directory `.pes/objects/XX/` (first 2 hex chars of hash).
5. Writes data to a temporary file, calls `fsync()`, then `rename()` for atomicity.
6. `fsync()`s the shard directory to persist the rename across crashes.

**`object_read`** retrieves and verifies stored data:

1. Builds the file path from the hash using `object_path()`.
2. Reads the entire file into a heap buffer.
3. Recomputes SHA-256 and compares against the filename — returns `-1` on mismatch (corruption detected).
4. Parses the type string (`blob`/`tree`/`commit`) and strips the header, returning only the data portion.

## Phase 2 — Tree Objects

### Implementation

**`tree_from_index`** builds a complete directory snapshot from the staging area. The key challenge is handling nested paths like `src/main.c` — these require creating subtree objects for each directory level.

The recursive helper `write_tree_level(entries, count, prefix, id_out)`:

1. Iterates through the sorted index entries for the current directory level.
2. For each entry, strips the current `prefix` to get the relative name.
3. If the relative name contains a `/`, the entry belongs in a subdirectory — extracts the immediate directory name, collects all entries sharing that sub-prefix, recurses to build and store the subtree, and adds a `040000` tree entry pointing to it.
4. If no `/` remains, it is a direct blob entry at this level.
5. Serializes the completed `Tree` struct and writes it via `object_write(OBJ_TREE, ...)`.

The `Index` struct (~5.3 MB) is heap-allocated to avoid stack overflow.

## Phase 3 — Staging Area (Index)

### Implementation

**`index_load`** reads `.pes/index` using `fscanf` line-by-line. A missing file initializes an empty index (not an error) to support fresh repositories.

**`index_save`** writes atomically:
1. Heap-allocates a copy of the index and sorts entries by path with `qsort`.
2. Opens a temp file, writes all entries in text format.
3. Calls `fflush` + `fsync` to commit buffered data to disk.
4. `rename()`s the temp file over the real index — atomic on POSIX.

**`index_add`** stages a file:
1. Reads the file's contents with `fopen`/`fread`.
2. Calls `object_write(OBJ_BLOB, ...)` to store the content and get its hash.
3. Reads file metadata via `lstat` (mode, mtime, size).
4. Updates the existing index entry for this path, or appends a new one, then calls `index_save`.

A `__attribute__((constructor))` in `index.c` raises the process stack limit to 64 MB before `main()` runs, because the provided `pes.c` allocates `Index` structs (~5.3 MB each) on the stack — exceeding the default 8 MB Linux limit.


## Phase 4 — Commits and History

### Implementation

**`commit_create`** ties the entire system together:

1. Calls `tree_from_index()` to build and store the directory snapshot, obtaining the root tree hash.
2. Calls `head_read()` to get the current HEAD commit hash as the parent (skipped for the first commit).
3. Fills a `Commit` struct with tree hash, optional parent, author from `pes_author()`, `time(NULL)` timestamp, and the commit message.
4. Calls `commit_serialize()` to produce the text-format commit object.
5. Calls `object_write(OBJ_COMMIT, ...)` to store it and obtain the new commit hash.
6. Calls `head_update()` to atomically move the branch pointer to the new commit.


## Implementation Notes

### Key Design Decisions

**Atomic writes everywhere.** Both `object_write` and `index_save` use the temp-file-then-`rename()` pattern. On POSIX systems `rename()` is atomic — a reader always sees either the old complete file or the new complete file, never a partial write. Combined with `fsync()` before rename, this survives crashes and power loss.

**Content-addressable deduplication.** Because objects are named by their SHA-256 hash, storing the same file content twice (across commits or across files) results in a single blob object. The three-commit sequence in Phase 4 produces only 10 objects for 3 commits × 3 files, because `file2.txt` is unchanged and shared by all three commit trees.

**Stack overflow mitigation.** The provided `pes.c` allocates `Index` structs (~5.3 MB each) on the stack, exceeding the default 8 MB Linux stack limit. A `__attribute__((constructor))` function in `index.c` raises the process stack limit to 64 MB via `setrlimit(RLIMIT_STACK)` before `main()` runs. `tree_from_index` also heap-allocates its `Index` copy independently.

**Integrity verification on every read.** `object_read` recomputes the SHA-256 of every file it reads and compares it against the hash embedded in the two-level path. Silent bit-rot or accidental corruption is detected immediately and returned as a read error rather than silently serving corrupt data.

### Building

```bash
sudo apt update && sudo apt install -y gcc build-essential libssl-dev
export PES_AUTHOR="Your Name <SRN>"
make all          # builds pes, test_objects, test_tree
make test         # runs unit tests + integration test
```