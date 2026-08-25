# ADR: Lifecycle Layer

## Status: Accepted

## Decision Drivers
- FPM master needs to spawn a governance worker (independent PHP CLI process) without crashing on failure
- Governance worker must be monitored and restarted if it crashes, with a retry limit to prevent resource exhaustion
- C code needs to invoke PHP callbacks (SPI pattern: C orchestrates, PHP implements) without crashing on callback exceptions
- Platform compatibility: Linux (prctl) and macOS (getppid fallback) for parent-death detection

## Context

Phase 3 implements the lifecycle layer: governance worker spawn/monitor/restart (`beacon_governance_worker.c`) and C→PHP callback invocation (`beacon_callback.c`). This sits above the domain service layer (Phase 2) and below the PHP API layer (Phase 4).

The core challenge: the governance worker is a long-lived background process that maintains the shm node table. It must be spawned by FPM master during MINIT, monitored for liveness, and restarted if it crashes. Additionally, the governance worker needs to invoke PHP callbacks (on_register, on_keepalive, on_discover, on_deregister, on_watch) which are user-supplied code that may throw exceptions.

## Decisions

### D23: fork + exec (not pure fork) for governance worker spawn

The governance worker is spawned via `fork()` + `execl()`, not pure `fork()`.

**Rationale:** PHP VM is a singleton — `fork()` inherits the parent's polluted VM state (loaded classes, ini settings, object stores). `exec()` replaces the address space entirely, starting a clean PHP VM. Without `exec()`, the child process would have corrupted VM state.

**Industry benchmark:**
- systemd: `fork()` + `exec()` for service startup
- Swoole: `fork()` for C-level workers (no PHP VM pollution), but `swoole_process` uses `exec` for PHP-level processes
- nginx: `fork()` without `exec()` (C workers, no VM)

**Alternatives considered:**
- Pure `fork()`: PHP VM state corruption — rejected
- `posix_spawn()`: More portable but less control over fd cleanup timing — rejected for simplicity

### D24: prctl(PR_SET_PDEATHSIG) + getppid() dual parent-death detection

On Linux, `prctl(PR_SET_PDEATHSIG, SIGTERM)` asks the kernel to automatically send SIGTERM to the child when the parent exits. On macOS (no prctl), the governance worker self-checks `getppid()` periodically.

**Rationale:** During FPM graceful reload, the old master exits and a new master starts. Without parent-death detection, the old governance worker becomes an orphan and continues running indefinitely. prctl provides kernel-level reliability; getppid() is a user-space fallback.

**Industry benchmark:**
- systemd: uses `PR_SET_PDEATHSIG` for worker processes
- Swoole: uses `getppid()` polling in worker main loop
- nginx: master monitors workers via `SIGCHLD` (opposite direction)

**Alternatives considered:**
- prctl only: Not portable to macOS — rejected
- getppid() only: Less reliable (race condition between parent death and next poll) — rejected as sole mechanism
- SIGCHLD handler in master: Already used for worker exit detection, but governance worker is not a direct child of FPM workers

### D25: Environment variable guard (BEACON_GOVERNANCE_WORKER=1) prevents infinite fork loop

Before `execl()`, the parent sets `BEACON_GOVERNANCE_WORKER=1` in the child's environment. The child's MINIT checks this env var and skips `beacon_governance_spawn()` if set.

**Rationale:** Without this guard, the child process's MINIT would call `beacon_governance_spawn()` again, creating an infinite fork loop that exhausts PIDs.

**Industry benchmark:**
- systemd: `ConditionEnvironment` for conditional execution
- nginx: `env` guard variables for worker processes

**Alternatives considered:**
- Check `getppid() == 1` (init): Unreliable — parent might not be init yet
- Check shm for existing pid: Race condition between MINIT and shm init
- Command-line argument: Works but env var is simpler and survives exec

### D26: Restart retry limit 5, then degraded mode

If the governance worker crashes, `beacon_governance_ensure_running()` spawns a new one. After 5 consecutive restart failures, it enters degraded mode (stops restarting, logs error).

**Rationale:** A governance worker that crashes immediately after spawn (e.g., bad governance script, missing PHP binary) would cause an infinite restart loop, exhausting system resources. The retry limit of 5 provides a reasonable window for transient failures while preventing resource exhaustion.

**Industry benchmark:**
- systemd: `StartLimitBurst=5` + `StartLimitAction=none` (stop restarting after N failures)
- nginx: `worker_processes` auto-restart without limit (but nginx workers are C-level, not PHP VM)
- supervisord: `startretries=3` default

**Alternatives considered:**
- Unlimited restarts: Resource exhaustion risk — rejected
- Retry limit 3: Too aggressive — transient failures (e.g., shm not yet ready) need more retries
- Retry limit 10: Too lenient — wastes resources before giving up
- Exponential backoff: Over-engineering for Phase 3; can be added later

### D27: No C-level timeout for callbacks (ualarm removed)

The C layer does not impose a timeout on PHP callback execution. It measures duration and logs a WARN if > 500ms, but does not interrupt.

**Rationale:** Signal-based timeout (`ualarm`) is not async-signal-safe — interrupting a PHP callback mid-execution can leave the VM in an inconsistent state. The PHP layer self-manages timeout via ReactPHP event loop and HTTP client timeouts. The C layer's role is "gentle hint" (measure + warn), not enforcement.

**Industry benchmark:**
- Swoole: `swoole_timer_tick` for PHP-level timeout, no C-level signal interruption
- PHP `register_shutdown_function`: no timeout, relies on PHP self-management
- PHP `max_execution_time`: uses `SIGPROF` but only in non-CGI SAPI, and is known to be unreliable with custom extensions

**Alternatives considered:**
- `ualarm()` + signal handler: Not async-signal-safe, can corrupt VM state — rejected
- `setitimer()` + signal handler: Same issue — rejected
- Thread-based watchdog: Over-engineering, adds threading complexity — rejected
- PHP `max_execution_time`: Not applicable to CLI SAPI (governance worker)

### D28: zend_try for callback exception isolation

PHP callback invocation is wrapped in `zend_try` / `zend_catch` / `zend_end_try` to prevent callback exceptions from crashing the governance worker.

**Rationale:** User-supplied callbacks may throw exceptions or trigger fatal errors. Without isolation, a single callback exception would crash the governance worker, triggering a restart cycle. `zend_try` catches fatal errors at the C level, allowing the governance worker to continue running.

**Industry benchmark:**
- Swoole: `try-catch` in `swoole_call_function` for exception isolation
- PHP `user_shutdown_function_call`: wraps in `zend_try` for shutdown function isolation
- PHP `zend_call_function`: does NOT isolate by default (caller's responsibility)

**Alternatives considered:**
- No isolation: One callback exception crashes governance worker → restart cycle — rejected
- `setjmp`/`longjmp` directly: Reimplemented `zend_try` — rejected (reinventing the wheel)

### D29: Callback storage as static zval array (not HashTable)

Callbacks are stored in a static `zval callback_storage[BEACON_CB_MAX]` array, indexed by `beacon_callback_type_t` enum value.

**Rationale:** Callback types are a finite set (5 types: ON_REGISTER/KEEPALIVE/DISCOVER/DEREGISTER/WATCH). Array access is O(1) with zero hash overhead. HashTable would add unnecessary complexity for 5 entries.

**Industry benchmark:**
- PHP `user_shutdown_function_list`: static array (not HashTable)
- Swoole: uses HashTable for callbacks (but supports unlimited callback types)

**Alternatives considered:**
- HashTable: Over-engineering for 5 fixed types — rejected
- Individual static variables: Less uniform, harder to iterate — rejected

## Consequences

- Governance worker spawn adds `fork()` + `exec()` overhead to MINIT (one-time, acceptable)
- RINIT adds `kill(pid, 0)` liveness check every 100 requests (lightweight system call)
- MSHUTDOWN adds `SIGTERM` + `waitpid` + `SIGKILL` fallback for governance worker cleanup
- Callback invocation adds `zend_try` overhead (setjmp on entry, negligible)
- `memset(&fci, 0, sizeof(fci))` is required before `zend_fcall_info` initialization — uninitialized `named_params` field causes segfault (learned during testing)
- Degraded mode (restart limit reached) means FPM continues serving with stale shm data — acceptable per D17 (governance down → DEGRADED, not DEAD)

## Related

- Source: `beacon_governance_worker.c`, `beacon_callback.c`, `php_beacon.h`, `beacon.c`, `config.m4`
- Design: `docs/design/governance-worker.md` (spawn mechanism, lifecycle state machine), `docs/design/spi.md` (SPI callback pattern)
- OpenSpec: `openspec/changes/2026-08-25-lifecycle-layer/`
- Tests: `tests/phase3_lifecycle.php` (11/11 passed)
