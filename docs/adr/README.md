# ADR Index

This directory contains Architecture Decision Records (ADRs) for php-beacon-extension.

## ADR Format

Each ADR file documents design decisions with:
- **Status**: Accepted / Proposed / Deprecated
- **Decision Drivers**: Why this decision was made
- **Context**: Background and constraints
- **Decisions**: The actual choices, with industry benchmark and alternatives
- **Consequences**: Impact of the decisions
- **Related**: Source files and design documents

## Index

### build-system.md
- D1: autoconf config.m4 (probes PHP version, system features, zlib)
- D2: config.h include pattern (PHP extension standard, required for PHP header macros)
- D3: _Alignof instead of alignof (C11 compliance, overriding PHP's -std=gnu23)

### module-globals.md
- D4: ZEND_BEGIN_MODULE_GLOBALS + ZEND_EXTERN_MODULE_GLOBALS (cross-TU visibility)
- D5: BEACON_G macro (beacon_globals.v, not beacon_globals.id)
- D6: INI directives with PHP_INI_SYSTEM (infrastructure-level, not runtime-changeable)

### shared-memory.md
- D7: sysv shmget/shmat (cross-process shared memory)
- D8: Double-buffer RCU (lock-free reads, write to inactive buffer + switch)
- D9: Packed C structs (deterministic layout across processes)
- D10: 64-byte aligned heartbeat slots (cache line, prevent false sharing)
- D11: C11 stdatomic for self-counting (portable atomic operations)
- D12: CRC32 checksum + memory barrier (corruption resilience)

### logging-error-handling.md
- D13: Dual logging strategy (php_error_docref for FPM, custom log for governance worker)
- D14: Log levels (DEBUG/INFO/WARN/ERROR, INI-driven filtering)
- D15: Defensive logging — never crash (degraded mode on failure)

### domain-service.md
- D16: Four-state health model (NOT_READY/OK/DEGRADED/DEAD, extends K8s/Envoy three-state)
- D17: Governance worker down → DEGRADED not DEAD (control plane vs data plane separation)
- D18: Saturation threshold 0.9 (90% busy triggers degradation, 10% headroom for bursts)
- D19: Per-process round-robin counter (not cross-process shared, law of large numbers)
- D20: Zero-copy node selection (return shm pointer, no allocation in request path)
- D21: Two-phase node filtering (OK first, DEGRADED second, prefer_healthy support)
- D22: Switch-case LB dispatch (finite set, not function pointer injection)

### lifecycle-layer.md
- D23: fork + exec (not pure fork) for governance worker spawn (PHP VM singleton)
- D24: prctl(PR_SET_PDEATHSIG) + getppid() dual parent-death detection (Linux/macOS)
- D25: Environment variable guard (BEACON_GOVERNANCE_WORKER=1) prevents infinite fork loop
- D26: Restart retry limit 5, then degraded mode (systemd StartLimitBurst benchmark)
- D27: No C-level timeout for callbacks (ualarm removed, PHP layer self-manages)
- D28: zend_try for callback exception isolation (Swoole/PHP shutdown function benchmark)
- D29: Callback storage as static zval array (finite set, not HashTable)

### php-api-layer.md
- D30: Class constants only (zend_declare_class_constant_long/string, not global REGISTER_LONG_CONSTANT)
- D31: Static methods only (ZEND_ACC_STATIC, no instances — utility class pattern)
- D32: String status mapping (C numeric ↔ PHP string, health vs node status different numeric values)
- D33: setOpt dispatch pattern (switch-case by key, curl/Redis/Yar benchmark)
- D34: pick() opts as integer-keyed array (CURLOPT_* pattern, zend_hash_index_find)
- D35: Error handling — return null/false, no exceptions (PHP extension convention)

## Design Documents

Detailed design documents are in `docs/design/` (13 files covering architecture, MVP plan, shm design, API reference, etc.).
