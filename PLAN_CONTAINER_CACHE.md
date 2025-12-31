# Plan: `--container-cache` (Module 3A) — Persistent RootFS Reuse with Pyxis + Enroot

This file is a working plan/spec for implementing and validating **Module 3A** in Pyxis:

- New user-facing flag: `srun --container-cache`
- Goal: reuse the **unpacked** Enroot rootfs across jobs on the **same node** to achieve near-1s warm starts
- Constraints:
  - Must work with the existing cluster cleanup behavior (Epilog deletes `pyxis_${SLURM_JOB_ID}*`)
  - **Disallow** `--container-writable` and `--container-save` in cache mode (to avoid unsafe cross-job state)
  - Must not require users to encode digest/version into `--container-name`

---

## Background / Why this exists

On our cluster:
- Cold start (`--container-name` first use) costs ~14–18s/node
- Warm start can be ~1s **only if** the unpacked rootfs persists
- Today rootfs does **not** persist across jobs because:
  - job-scoped naming (`pyxis_${JOBID}_...`) and
  - `/etc/slurm/epilog.d/70-enroot-container-cleanup.sh` removing `pyxis_${SLURM_JOB_ID}*`

So we need a **Pyxis-native** mechanism to:
1) generate stable cache identities and
2) ensure the resulting rootfs directories do not match job-scoped cleanup patterns.

---

## Design summary

### User interface

- **Flag:** `--container-cache`
  - Also available via env: `PYXIS_CONTAINER_CACHE=1`
- When set:
  - Requires `--container-image`
  - Rejects `--container-writable`
  - Rejects `--container-save`
  - Forces read-only rootfs (`ENROOT_ROOTFS_WRITABLE=n`)

### How caching works

1) **Stable cache key** is derived from the image identity:
   - For `.sqsh` paths: `abs_path + mtime + size` (fast; avoids hashing huge files)
   - For non-path images: hash the image string (future improvement: OCI digest)

2) **Stable container name** is auto-generated from the cache key:
   - Example prefix: `pyxis_cache_<uid>_<hash>`
   - **Important:** In cache mode, naming must be **non-job-scoped** (no jobid prefix).

3) **Reuse behavior**
   - If the container already exists: Pyxis reuses the filesystem and skips `enroot create`
   - Otherwise: Pyxis creates it once (cold path), then it persists

### Cache directory layout

- **Required config for cache mode** (plugstack arg):
  - `container_cache_data_path=/raid/containers/data`
- **Per-user cache directory**:
  - Pyxis creates/uses `<base>/<uid>` with mode `0700` and ownership `<uid>:<gid>`
  - For cache mode, Pyxis sets `ENROOT_DATA_PATH=<base>/<uid>` for all Enroot calls
- **Cached rootfs directory name**:
  - `pyxis_cache_u<uid>_<hash>`
  - Full path: `<base>/<uid>/pyxis_cache_u<uid>_<hash>`
- **Locking + "last used" signal**:
  - Pyxis creates `<rootfs>/.pyxis_cache_lock`
    - jobs hold a **shared** lock for the job lifetime
    - GC tries an **exclusive non-blocking** lock; if it can’t lock, the entry is treated as in-use
  - Each time a cached rootfs is used, Pyxis `touch`es the rootfs directory to update its `mtime`
    - GC uses this `mtime` as the LRU timestamp (no reliance on filesystem `atime`)

### GC / LRU eviction (global across users)

Goal: prevent jobs failing due to the cache filesystem being full.

- **When GC runs**:
  - Opportunistic, at job start in cache mode **only if** we’re about to create a new cached rootfs (cold path)
  - Reuse path should not trigger GC
- **Watermarks** (admin-configurable):
  - `container_cache_gc_high=85` (default): start evicting when used% is \(\ge\) high
  - `container_cache_gc_low=80` (default): stop evicting when used% drops below low
- **Global serialization**:
  - GC takes an exclusive lock on `<base>/pyxis-container-cache-gc.lock` so multiple jobs don’t evict concurrently
- **Candidate selection (LRU)**:
  - Scan all user dirs: `<base>/*/pyxis_cache_*`
  - Sort candidates by directory `mtime` (oldest first)
- **Eviction loop**:
  - For each candidate, try to acquire an **exclusive non-blocking** lock on `<candidate>/.pyxis_cache_lock`
    - if locked: recursively delete the candidate directory
    - if not lockable: skip (in-use)
  - Re-check used% and stop once below `container_cache_gc_low`
- **Cross-user behavior**:
  - Because `<base>/<uid>` is `0700`, GC must run in a privileged SPANK hook so it can traverse/evict other users’ entries.

---

## Code changes (high-level)

### 1) Add new flag and env var plumbing
- `args.h`: add `int container_cache;`
- `args.c`:
  - register `--container-cache`
  - parse `PYXIS_CONTAINER_CACHE`

### 2) Implement cache mode in `pyxis_slurmstepd.c`
- During `slurm_spank_user_init`:
  - validate incompatible flags (`--container-writable`, `--container-save`)
  - compute stable name
  - force `container_scope=global` for cache mode
  - derive a per-user cache directory from `container_cache_data_path` (`<base>/<uid>`)

- During Enroot execution (`enroot_set_env`):
  - set `ENROOT_DATA_PATH` for cache mode (`<base>/<uid>`)

- During container create:
  - run GC if needed (global `/raid` thresholds)
  - touch/lock cached entries to prevent eviction while in-use

### 3) Config knobs
Extend `config.[ch]` to parse:
- `container_cache_data_path=...`
- `container_cache_gc_high=...`
- `container_cache_gc_low=...`

---

## Testing plan (cluster)

### Automated (BATS)
- Ensure Slurm env is set (cluster-specific; adjust paths as needed):

```bash
export SLURM_ROOT=/cm/local/apps/slurm/24.11
export PATH="$SLURM_ROOT/bin:$SLURM_ROOT/sbin:$PATH"
export LD_LIBRARY_PATH="$SLURM_ROOT/lib64:${LD_LIBRARY_PATH:-}"
export SLURM_CONF=/etc/slurm/slurm.conf
```

- Run: `bats tests/container_cache.bats`
  - Covers: policy enforcement, stable naming/layout under `<base>/<uid>`, read-only enforcement, env var enablement, and GC behavior (including cross-user eviction) when usage is above the configured high watermark.
  - If needed: `PYXIS_TEST_SQSH_IMAGE=/path/to/image.sqsh bats tests/container_cache.bats`

### Functional correctness (manual)
1) Cold create:
   - `srun --container-cache --container-image=<image> ...`
   - expect: rootfs directory `<base>/<uid>/pyxis_cache_u<uid>_<hash>` is created
2) Warm reuse (separate job, same node):
   - run the same command on the same node
   - expect: the same cached rootfs is reused (near-1s startup)

### Cleanup compatibility
- Verify cached directories are **non-job-scoped** (e.g. `pyxis_cache_u<uid>_*`) and do **not** match Epilog `pyxis_${JOBID}*` patterns.

### GC/LRU (manual)
- Trigger GC by filling the cache filesystem above `container_cache_gc_high`, then create new cached rootfs entries.
- Confirm oldest caches are evicted first and locked/in-use caches are not evicted.

---

## Open items / future improvements

- Use image digest for OCI images (instead of hashing only the image string)
- More robust last-used tracking (avoid relying on directory `mtime`)
- Expand test coverage for concurrency/stress scenarios on a multi-node cluster

---


