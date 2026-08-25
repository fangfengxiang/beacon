# ADR: Shared Memory Double-Buffering

## Status: Accepted

## Decision Drivers
- FPM workers (short-lived, per-request) need lock-free reads of service node tables
- Governance worker (long-lived) writes service node tables periodically
- No mutex in FPM request path (performance critical)
- Cross-process shared state (master + workers + governance worker)

## Context

PHP-FPM's process model creates a fundamental tension: FPM workers are short-lived (per-request lifecycle) but service governance requires long-lived state (service registry, health status, node tables). The extension uses sysv shared memory (shm) to bridge this gap.

The core challenge is concurrent read/write: governance worker writes service node tables while FPM workers read them. Traditional mutex locks are too expensive for the FPM request path (RINIT/RSHUTDOWN).

## Decisions

### D7: sysv shmget/shmat

Use sysv shared memory (`shmget` + `shmat`) for cross-process shared state.

- `shmget(key, size, IPC_CREAT | IPC_EXCL | 0666)` — create new segment
- `shmget(key, size, 0666)` — attach existing segment (FPM reload scenario)
- Magic number (`0x42454143` = 'BEAC') validates segment identity (prevents key conflict)
- Only the creating process (FPM master) calls `IPC_RMID` on MSHUTDOWN

**Industry benchmark:** Swoole Table uses `shmget` + `shmat` for cross-process shared memory. Swoole Atomic uses `shmget` for cross-process atomic counters.

**Alternatives considered:**
- POSIX `mmap` + `MAP_SHARED`: More portable but requires file descriptor management
- `mmap` + `/dev/shm`: Linux-specific, not portable to macOS
- APCu shared memory: PHP-level, too high overhead for C extension

### D8: Double-buffer RCU (Read-Copy-Update)

Two buffers (`buffer_a`, `buffer_b`) in the shm segment. An `active` field in the header indicates which buffer is currently active for reading.

**Write path (governance worker):**
1. Write to inactive buffer
2. `__sync_synchronize()` (memory barrier)
3. Switch `active` field
4. `__sync_synchronize()` (ensure switch visible)

**Read path (FPM worker):**
1. Read `active` field
2. Read from active buffer
3. If primary buffer empty/corrupted, fall back to backup buffer

This is RCU (Read-Copy-Update) pattern: readers never block, writers never block readers.

**Industry benchmark:** Linux kernel RCU (`rcu_read_lock` / `synchronize_rcu`), Swoole Table (double-buffer for atomic reads).

**Alternatives considered:**
- Mutex (pthread `pthread_mutex_t` with `PTHREAD_PROCESS_SHARED`): Blocks readers during write
- Seqlock: Requires retry on read, more complex
- Single buffer + atomic version: No fallback on corruption

### D9: Packed C structs

All shm structs use `__attribute__((packed))` to ensure deterministic memory layout across processes and platforms.

```c
typedef struct __attribute__((packed)) {
    char name[64];
    beacon_node_t nodes[16];
    uint32_t node_count;
    uint32_t version;
    uint8_t  writing;
} beacon_service_t;
```

**Industry benchmark:** Swoole Table uses explicit `#pragma pack(push, 1)` for shared memory structs. Network protocol headers use `__attribute__((packed))`.

### D10: 64-byte aligned heartbeat slots (cache line)

```c
typedef struct __attribute__((aligned(64))) {
    uint32_t pid;         /* offset 0-3 */
    uint32_t _pad0;       /* offset 4-7 */
    uint64_t last_rinit;  /* offset 8-15 */
    uint8_t  busy;        /* offset 16 */
    uint8_t  padding[47]; /* offset 17-63 */
} beacon_worker_slot_t;   /* 64 bytes, aligned 64 */
```

64-byte alignment matches the CPU cache line size (x86_64 and ARM64). This prevents false sharing: when one FPM worker writes to its slot, it doesn't invalidate another worker's cache line.

`_Static_assert` verifies size and alignment at compile time.

**Industry benchmark:** Linux kernel `____cacheline_aligned` (64 bytes), Swoole `SW_CACHE_LINE_SIZE` (64 bytes).

### D11: C11 stdatomic for self-counting

```c
#include <stdatomic.h>
atomic_fetch_add(&shm->header.pool_busy, 1);  /* RINIT */
atomic_fetch_sub(&shm->header.pool_busy, 1);  /* RSHUTDOWN */
atomic_fetch_add(&shm->header.pool_total, 1);
```

C11 `<stdatomic.h>` provides portable atomic operations. On x86_64, `atomic_fetch_add` compiles to a single `LOCK XADD` instruction. On ARM64, it uses `LDXR`/`STXR` (load-exclusive/store-exclusive).

**Industry benchmark:** Swoole Atomic uses `__sync_fetch_and_add` (GCC builtins). Linux kernel uses `atomic_inc` / `atomic_add`. C11 `<stdatomic.h>` is the portable standard.

**Alternatives considered:**
- GCC `__sync` builtins: Not portable to non-GCC compilers
- Inline assembly: Architecture-specific, unmaintainable
- `volatile` only: Not atomic, race conditions

### D12: CRC32 checksum + memory barrier

Header contains a CRC32 checksum of the active buffer. On read, if CRC32 doesn't match, fall back to backup buffer.

```c
__sync_synchronize();  /* memory barrier before switching active */
shm->header.active = inactive;
__sync_synchronize();  /* memory barrier after switching active */
```

`__sync_synchronize()` is a full memory barrier (equivalent to x86 `MFENCE` or ARM `DMB ISH`). It ensures all writes before the barrier are visible to all CPUs before the `active` switch.

CRC32 uses zlib's `crc32()` if available, otherwise a built-in table-driven implementation.

**Industry benchmark:** Swoole Table uses checksum for shared memory integrity. Linux kernel uses `smp_mb()` for RCU memory ordering.

## Consequences

- FPM workers read service node tables with zero locking (RCU pattern)
- Governance worker writes without blocking readers
- 64-byte alignment prevents false sharing in heartbeat slots
- C11 atomics provide portable, efficient self-counting
- CRC32 + double-buffer provides corruption resilience
- sysv shm is portable across macOS and Linux

## Related
- `beacon_shm.h` — struct definitions
- `beacon_shm.c` — implementation
- `docs/design/shm-design.md` — detailed design document
