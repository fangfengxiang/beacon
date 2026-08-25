# ADR: Domain Service Layer

## Status: Accepted

## Decision Drivers
- FPM workers need fast, lock-free node selection in the request path (RINIT → pick → RSHUTDOWN)
- Health state must reflect both data plane (FPM pool) and control plane (governance worker) status
- LB strategies are a finite set (round_robin/random/weighted), not an extensible plugin system
- Zero allocation in request path (design principle: zero I/O + zero memory allocation)

## Context

Phase 2 implements the domain service layer: health calculation (`beacon_service_health.c`) and node selection (`beacon_service_select.c`). These sit above the infrastructure layer (shm) and below the lifecycle layer (Phase 3) and PHP API layer (Phase 4).

The core challenge: FPM workers need to make sub-millisecond node selection decisions using shm-cached data, while the governance worker (long-lived background process) maintains that data. Health calculation runs in the governance worker's keepalive tick; node selection runs in every FPM request that calls `Beacon::pick()`.

## Decisions

### D16: Four-state health model (NOT_READY/OK/DEGRADED/DEAD)

Health status uses four states, not the typical two-state (up/down) or three-state (healthy/degraded/unhealthy) model:

```
NOT_READY → pool_ready == 0 (Beacon::ready() not called, warmup incomplete)
OK        → pool_ready, governance alive, saturation < 0.9
DEGRADED  → governance down OR saturation >= 0.9
DEAD      → (reserved for future: FPM master down, not used in Phase 2)
```

Priority chain (highest to lowest): NOT_READY > DEGRADED > OK. DEAD is terminal (not auto-recovered).

**Industry benchmark:**
- gRPC Health Checking Protocol (`grpc.health.v1.Health`): two-state (`SERVING`/`NOT_SERVING`)
- K8s probes: three-state (`passing`/`warning`/`critical`) + startup state
- Envoy health check: three-state (`healthy`/`degraded`/`unhealthy`)

Beacon's four-state model extends K8s/Envoy with `NOT_READY` (startup/warmup phase) — analogous to K8s startup probe but integrated into the health state machine rather than a separate probe type.

**Alternatives considered:**
- Two-state (up/down): Too coarse — cannot express "governance down but FPM still serving"
- Three-state: Cannot distinguish "not ready" (startup) from "degraded" (partial failure)

### D17: Governance worker down → DEGRADED, not DEAD

When the governance worker (control plane) is down but FPM workers (data plane) are still alive, health status is `DEGRADED`, not `DEAD`.

**Rationale:** The governance worker handles registration/keepalive/discovery. If it crashes, FPM workers can still serve requests using the shm-cached node table. Reporting `DEAD` would cause the registry to deregister this instance, but FPM is actually still serving — this is a false deregistration. `DEGRADED` causes the registry to lower the instance weight (less traffic) rather than remove it, which is the correct behavior.

**Industry benchmark:**
- K8s: kubelet down does not immediately remove Pods (grace period). The node is cordoned (no new pods) but existing pods keep running.
- Envoy: control plane (xDS server) down does not remove clusters — Envoy uses cached config and continues serving.

**Alternatives considered:**
- Report `DEAD`: Causes false deregistration, FPM still serving → incorrect
- Report `OK`: Hides the problem — stale node data, no governance → misleading

### D18: Saturation threshold 0.9 (90% busy)

`BEACON_SATURATION_DEGRADED_THRESHOLD = 0.9` — when 90% of pool workers are busy, health degrades to `DEGRADED`.

**Rationale:** 90% busy means the pool is near saturation; new requests may queue. The 10% headroom absorbs traffic bursts. Below 90%, the pool has sufficient capacity.

**Industry benchmark:**
- Envoy `panic_threshold`: default 50% (if >50% of nodes are unhealthy, route to all nodes including unhealthy ones). Different metric (node health, not pool saturation) but same concept of threshold-based degradation.
- nginx `max_fails`: count-based, not ratio-based. Less precise for saturation detection.

**Alternatives considered:**
- 1.0 (100%): Too late — by the time all workers are busy, requests are already queuing
- 0.8 (80%): Too aggressive — normal traffic spikes would trigger degradation
- Configurable via INI: Rejected for Phase 2 (YAGNI); can be added later if needed

### D19: Per-process round-robin counter (not cross-process shared)

The round-robin LB strategy uses a per-FPM-worker `static uint32_t rr_counter`, not a cross-process shared counter in shm.

**Rationale:**
- Cross-process shared counter requires shm atomic operations, adding overhead to every `pick()` call
- Each FPM worker independently cycling through nodes produces statistically uniform distribution (law of large numbers across many workers)
- Simpler implementation — no shm contention, no lock

**Industry benchmark:**
- nginx: each worker process maintains its own round-robin counter
- Envoy: per-thread LB state (not cross-thread shared)

**Alternatives considered:**
- shm atomic counter: More precise distribution but adds atomic operation overhead to every pick() — rejected for performance
- Per-worker shm slot counter: Would work but requires shm write on every pick — rejected for performance

### D20: Zero-copy node selection (return shm pointer)

`beacon_select_pick()` returns a `const beacon_node_t *` pointing directly into shm, not a copy.

**Rationale:**
- Zero memory allocation in the request path (design principle)
- shm data is immutable between commits (governance worker writes to inactive buffer, then atomically switches)
- The PHP API layer (Phase 4) will immediately copy the pointer data into a PHP array for userland, so the pointer lifetime is bounded to a single function call

**Industry benchmark:**
- nginx upstream: returns pointer to shared memory node, caller copies if needed
- gRPC client LB: returns `SubchannelConn` pointer, not copy

**Alternatives considered:**
- Copy to caller-allocated buffer: Adds memcpy overhead, unnecessary since caller will copy to PHP array anyway
- Return by value: `beacon_node_t` is ~392 bytes, too large for stack return

### D21: Two-phase node filtering (OK first, DEGRADED second)

Node filtering collects OK nodes first, then DEGRADED nodes. When `prefer_healthy=true` and OK nodes exist, DEGRADED nodes are excluded entirely.

**Rationale:**
- `prefer_healthy=true` means "prefer healthy nodes" — if healthy nodes exist, don't use degraded ones
- `prefer_healthy=false` means "use any non-dead node" — OK and DEGRADED are both acceptable
- Two-phase (rather than sorting) avoids O(n log n) sort; collection is O(n)

**Industry benchmark:**
- Envoy: priority-based LB (P=0 preferred, P=1 overflow) — similar two-tier approach
- gRPC: no prefer_healthy concept; relies on health check to remove unhealthy nodes

**Alternatives considered:**
- Sort by status then pick: O(n log n) per pick — unnecessary overhead for small n (≤64)
- Single-phase with status weighting: More complex, harder to reason about

### D22: Switch-case LB dispatch (not function pointer injection)

LB strategy selection uses `switch(strategy)`, not function pointer injection.

**Rationale:**
- Three strategies (round_robin/random/weighted) are a finite, closed set — not an extension point
- Switch-case is more readable and optimizable than function pointer dispatch
- Adding a new strategy requires modifying one switch statement, not registering a plugin

**Industry benchmark:**
- nginx: uses `switch` for LB method dispatch (not function pointers)
- Envoy: uses virtual inheritance (C++), but that's a different language with different tradeoffs

**Alternatives considered:**
- Function pointer table: Over-engineering for 3 strategies; adds indirection overhead
- Strategy pattern with struct: C doesn't have first-class strategy pattern; would be function pointers in disguise

## Consequences

- Health calculation is O(1) per call (read atomics + compare thresholds)
- Node selection is O(n) where n ≤ 64 (BEACON_MAX_NODES_PER_SVC) — bounded, no allocation
- Per-process round-robin may produce slightly uneven distribution across FPM workers, but converges to uniform at scale
- Zero-copy return requires callers to not hold the pointer across a `beacon_shm_commit()` call (documented constraint)
- `DEAD` state is reserved but not implemented in Phase 2 — will be used in Phase 3 (governance worker lifecycle)

## Related

- Source: `beacon_service_health.c`, `beacon_service_select.c`, `php_beacon.h`
- Design: `docs/design/spi.md` §三 (health checker SPI), `docs/design/api-reference.md` §1.1-1.3 (pick/getInstances API), `docs/design/shm-design.md` §4.4 (calibration timing)
- OpenSpec: `openspec/changes/2026-08-25-domain-service-layer/`
- Tests: `tests/phase2_domain.php` (28/28 passed)
