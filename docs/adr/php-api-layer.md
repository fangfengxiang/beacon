# ADR: PHP API Layer

## Status: Accepted

## Decision Drivers
- PHP business code needs a userland API to access beacon capabilities (pick, getInstances, ready, status, setOpt, deregister, reportHealth)
- Governance worker needs a userland API to write shm (storeNodes, commit) and calculate health (calcHealth)
- API must follow PHP extension conventions: return null/false for state, no exceptions
- Constants must be accessible as class constants (Beacon::OPT_*) not global constants (Beacon\OPT_*)

## Context

Phase 4 implements the PHP userland API: `Beacon` class (`beacon_api.c`) and `Beacon\Governance` class (`beacon_api_governance.c`). This is the top layer that PHP business code calls. It sits above the lifecycle layer (Phase 3) and domain service layer (Phase 2).

The core challenge: the extension has internal C functions (`beacon_test_*`) for testing, but no proper PHP userland API. Phase 4 wraps the internal C functions into proper PHP classes with type-safe arginfo declarations, string status mapping, and error handling following PHP extension conventions.

## Decisions

### D30: Class constants (not global constants) for Beacon::OPT_*/LB_*/HEALTH_*

Constants are registered as class constants on the `Beacon` class using `zend_declare_class_constant_long` / `zend_declare_class_constant_string`, not as global constants with `REGISTER_LONG_CONSTANT`.

**Rationale:** PHP userland accesses constants as `Beacon::OPT_EXCLUDE` (class constant syntax), not `Beacon\OPT_EXCLUDE` (global constant with namespace). Class constants are the standard PHP extension pattern for option keys — `Redis::OPT_*`, `Yar_Client::OPT_PACKAGER` all use class constants. Global constants with backslash names are a PHP 4-era pattern that doesn't support `::` syntax.

**Industry benchmark:**
- Redis: `Redis::OPT_PREFIX` (class constant)
- Yar: `Yar_Client::OPT_PACKAGER` (class constant)
- curl: `CURLOPT_*` (global constant, but curl is a procedural API, not class-based)

**Alternatives considered:**
- Global constants only (`Beacon\OPT_EXCLUDE`): PHP 8.5 requires `Beacon::OPT_EXCLUDE` syntax for class constant access — global constants with backslash don't support `::` — rejected
- Both global + class constants: Redundant — rejected. Phase 2/3 test files updated to use class constant syntax (`Beacon::OPT_*`)

### D31: Static methods only (no instances) for Beacon and Beacon\Governance

Both `Beacon` and `Beacon\Governance` classes use static methods only (`ZEND_ACC_STATIC`). No instances can be created.

**Rationale:** Beacon is a utility class — all state is in shm (shared across processes), not per-instance. Creating instances would imply per-instance state, which doesn't exist. Static methods match the usage pattern: `Beacon::pick('user')` is called per-request without instantiation.

**Industry benchmark:**
- Swoole: `Swoole\Coroutine::create()` (static method)
- PHP: `PDO::getAvailableDrivers()` (static method on utility class)

**Alternatives considered:**
- Instance methods with singleton: Over-engineering, no per-instance state — rejected
- Procedural functions (`beacon_pick()`): Not OOP, doesn't support class constants — rejected

### D32: String status mapping (C numeric ↔ PHP string)

C layer uses numeric status codes (`BEACON_HEALTH_STATUS_OK = 1`, `BEACON_NODE_STATUS_OK = 0`), but PHP userland sees string constants (`Beacon::HEALTH_OK = "ok"`). The API layer maps between them.

**Rationale:** Numeric codes are efficient for C layer (switch-case, struct fields). String constants are self-documenting for PHP userland (no need to look up what "2" means). The API layer is the boundary — it converts numeric→string on output (pick, getInstances, status, calcHealth) and string→numeric on input (reportHealth, storeNodes).

**Key insight:** Health status codes and node status codes have different numeric values but map to the same string set. `BEACON_HEALTH_STATUS_OK = 1` but `BEACON_NODE_STATUS_OK = 0`. Both map to `"ok"`. This is intentional — health status is pool-level (computed), node status is per-instance (stored in shm).

**Industry benchmark:**
- gRPC Health: `"SERVING"` / `"NOT_SERVING"` (string status)
- K8s probe: `passing` / `warning` / `critical` (string status)
- Envoy: `healthy` / `degraded` / `unhealthy` (string status)

### D33: setOpt dispatch pattern (switch-case by key)

`Beacon::setOpt(int $key, mixed $value)` uses a switch-case to dispatch by key: callbacks (1-5) → `beacon_callback_set`, LB strategy (6) → runtime override, intervals (7-10) → globals update, pick-only options (11-12) → reject with warning.

**Rationale:** The OPT_* keys are a finite set (12 values). Switch-case is the most direct dispatch mechanism. This mirrors the C layer's `beacon_select_pick` switch-case for LB strategies (D22).

**Industry benchmark:**
- curl: `curl_setopt(ch, option, value)` — switch-case dispatch by `CURLOPT_*`
- Redis: `Redis::setOption(key, value)` — switch-case dispatch by `Redis::OPT_*`
- Yar: `Yar_Client::setOpt(type, value)` — switch-case dispatch by `YAR_OPT_*`

**Alternatives considered:**
- HashTable dispatch: Over-engineering for 12 keys — rejected
- Function pointer array: Keys are not contiguous (1-12 but 11-12 are pick-only) — rejected

### D34: pick() opts as integer-keyed array

`Beacon::pick(string $service, array $opts = [])` uses integer-keyed array with `Beacon::OPT_*` constants as keys: `[Beacon::OPT_EXCLUDE => [...], Beacon::OPT_LB_STRATEGY => Beacon::LB_RANDOM]`.

**Rationale:** Integer keys match the `CURLOPT_*` pattern — the key is a numeric constant, not a string. This allows type-safe key access and prevents typos. `zend_hash_index_find` is used to look up integer keys.

**Industry benchmark:**
- curl: `curl_setopt_array(ch, [CURLOPT_URL => "...", CURLOPT_RETURNTRANSFER => true])` — integer-keyed array
- Redis: `Redis::pipeline([Redis::OPT_PREFIX => "..."])` — integer-keyed array

**Alternatives considered:**
- String-keyed array (`['exclude' => [...], 'lb_strategy' => ...]`): Less type-safe, prone to typos — rejected
- Named parameters (PHP 8 `pick(service, exclude: [], lb: ...)`): Not all options are needed every call, array is more flexible — rejected

### D35: Error handling — return null/false, no exceptions

All API methods return null/false for error states, never throw exceptions. Internal errors (shm unavailable) emit `php_error_docref` warnings.

**Rationale:** PHP extension convention — Redis returns `false` for missing keys, curl returns `false` for failed requests. Exceptions in extensions are rare and disruptive. Business code checks return values: `$node = Beacon::pick('user'); if ($node === null) { ... }`.

**Industry benchmark:**
- Redis: `$val = $redis->get('key');` — false = not found
- curl: `$res = curl_exec($ch);` — false = failed
- Yar: `$client->call(...)` — throws exception (but Yar is a client library, not a state beacon)

**Alternatives considered:**
- Exceptions (`throw new BeaconException`): Disruptive, not PHP extension convention — rejected
- Error objects: Over-engineering for Phase 4 — rejected

## Consequences

- MINIT gains class registration (`INIT_CLASS_ENTRY` + `zend_register_internal_class` + `zend_declare_class_constant_*`)
- PHP userland gains proper `Beacon` and `Beacon\Governance` classes with type-safe arginfo
- Internal test functions (`beacon_test_*`) retained for backward compatibility with Phase 2/3 tests
- Phase 2/3 test files updated to use class constant syntax (`Beacon::OPT_*`) instead of namespace constant syntax (`Beacon\OPT_*`)
- shm header gains `business_health_status` field (1 byte, from reserved space)
- `runtime_lb_strategy` moved to module globals for ZTS support
- Status mapping adds minimal overhead (switch-case, O(1))

## Related

- Source: `beacon_api.c`, `beacon_api_governance.c`, `php_beacon.h`, `beacon_shm.h`, `beacon.c`, `config.m4`
- Design: `docs/design/api-reference.md` (API specification), `docs/design/mvp.md` (Phase 4 plan)
- OpenSpec: `openspec/changes/2026-08-25-php-api-layer/`
- Tests: `tests/phase4_api.php` (75/75 passed), Phase 2 (28/28) and Phase 3 (11/11) tests passing with class constant syntax
