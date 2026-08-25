# ADR: Logging and Error Handling

## Status: Accepted

## Decision Drivers
- FPM worker request path must not crash on logging failure
- Governance worker (CLI SAPI) needs file-based logging
- PHP standard `php_error_docref` for request-path errors
- Log level filtering at runtime via INI

## Context

The extension has two runtime contexts:
1. **FPM worker** (per-request): Errors should use `php_error_docref` (PHP standard), which integrates with PHP's error handling. Logging to file in request path is too expensive.
2. **Governance worker** (CLI SAPI, long-lived): Needs file-based logging with timestamps, similar to a daemon log.

## Decisions

### D13: Dual logging strategy

- **FPM worker path:** `php_error_docref` (PHP standard) — integrates with PHP error handling, respects `display_errors` / `log_errors` INI
- **Governance worker path:** Custom `beacon_log_impl()` — writes to file (`beacon.log_file` INI) or stderr, with timestamp + level + file:line

**Industry benchmark:** Swoole uses `swLog` for daemon logging + `php_error_docref` for request-path errors. Yar uses `php_error_docref` exclusively.

### D14: Log levels (DEBUG/INFO/WARN/ERROR)

```c
#define BEACON_LOG_DEBUG 1
#define BEACON_LOG_INFO  2
#define BEACON_LOG_WARN  3
#define BEACON_LOG_ERROR 4
```

Level filtering: `if (level < configured_level) return;`

Default level: `WARN` (from `beacon.log_level` INI).

**Industry benchmark:** syslog (DEBUG/INFO/WARN/ERROR), Swoole `SW_LOG_*` (DEBUG/TRACE/INFO/NOTICE/WARN/ERROR).

### D15: Defensive logging — never crash

All shm operations that fail only log an error and return -1. The extension continues in degraded mode. This is critical because:
- MINIT crash = FPM master crash = all workers die
- RINIT crash = FPM worker crash = 502 to user
- MSHUTDOWN crash = FPM master crash on reload

```c
if (beacon_shm_init() != 0) {
    beacon_log(BEACON_LOG_ERROR, "shm init failed, extension running in degraded mode");
    /* 不返回 FAILURE——扩展降级，FPM 仍可服务请求 */
}
```

**Industry benchmark:** Swoole's defensive design (shm failure → degraded mode, not crash). PHP extension convention: MINIT returning FAILURE prevents module loading, but returning SUCCESS with degraded mode is safer for FPM.

## Consequences

- FPM worker request path uses PHP standard error handling
- Governance worker has file-based logging with timestamps
- All failures are logged, not crashed
- Extension degrades gracefully on shm failure

## Related
- `beacon_log.h` — log macros and level definitions
- `beacon_log.c` — log implementation
- `beacon.c` — defensive shm init in MINIT
