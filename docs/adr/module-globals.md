# ADR: Module Globals and INI Configuration

## Status: Accepted

## Decision Drivers
- PHP-FPM multi-process model: master + workers share extension state
- INI directives must be `PHP_INI_SYSTEM` (set in php.ini/FPM pool config, not runtime)
- Cross-TU (translation unit) access to globals requires extern declaration

## Context

PHP extensions use `ZEND_BEGIN_MODULE_GLOBALS` / `ZEND_END_MODULE_GLOBALS` to define a per-module globals struct. In non-ZTS mode (standard PHP-FPM), this creates a struct type and a global variable. The variable is defined in one .c file (`ZEND_DECLARE_MODULE_GLOBALS`) and must be declared extern in the header for other .c files to access.

## Decisions

### D4: ZEND_BEGIN_MODULE_GLOBALS + ZEND_EXTERN_MODULE_GLOBALS

`php_beacon.h` declares the globals struct and extern:
```c
ZEND_BEGIN_MODULE_GLOBALS(beacon)
    zend_bool   enabled;
    char       *service_name;
    /* ... 16 INI fields ... */
    int         shm_id;
    beacon_shm_t *shm;
    zend_bool   is_shm_owner;
    pid_t       governance_pid;
ZEND_END_MODULE_GLOBALS(beacon)
ZEND_EXTERN_MODULE_GLOBALS(beacon)
```

`beacon.c` defines the instance:
```c
ZEND_DECLARE_MODULE_GLOBALS(beacon)
```

**Key pitfall:** `ZEND_END_MODULE_GLOBALS` only creates the struct typedef, NOT the extern declaration. Without `ZEND_EXTERN_MODULE_GLOBALS`, other .c files (beacon_log.c, beacon_shm.c) cannot access `beacon_globals`, causing "undeclared identifier" errors.

**Industry benchmark:** Swoole (`SWOOLE_G` + `ZEND_EXTERN_MODULE_GLOBALS`), Yar (`YAR_G` + extern declaration)

### D5: BEACON_G macro

```c
#ifdef ZTS
#define BEACON_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(beacon, v)
#else
#define BEACON_G(v) (beacon_globals.v)
#endif
```

**Key pitfall:** The non-ZTS macro must use `beacon_globals.v` (parameter `v`), not `beacon_globals.id` (literal `id`). The `id` form is a bug that would always access the same field regardless of the parameter.

**Industry benchmark:** `YAR_G(v)` → `(yar_globals.v)`, `SWOOLE_G(v)` → `(swoole_globals.v)`

### D6: INI directives with PHP_INI_SYSTEM

All 16 INI directives use `PHP_INI_SYSTEM` access level:
```c
STD_PHP_INI_BOOLEAN("beacon.enabled", "0", PHP_INI_SYSTEM, OnUpdateBool, enabled, ...)
STD_PHP_INI_ENTRY("beacon.service_name", "", PHP_INI_SYSTEM, OnUpdateString, service_name, ...)
```

`PHP_INI_SYSTEM` means directives can only be set in `php.ini` or FPM pool config (`www.conf`), NOT via `ini_set()` at runtime. This is correct for FPM governance configuration — these are infrastructure-level settings that should not change per-request.

**Industry benchmark:** Swoole uses `PHP_INI_ALL` for most directives (runtime changeable). Yar uses `PHP_INI_ALL` for `yar.packager`. Our choice of `PHP_INI_SYSTEM` is stricter because beacon is infrastructure, not application-level.

**Alternatives considered:**
- `PHP_INI_ALL`: Allows `ini_set()` but risks per-request configuration drift in FPM workers
- `PHP_INI_PERDIR`: Allows `.htaccess` but FPM doesn't support `.htaccess`

## Consequences

- Globals are properly visible across all .c files
- INI directives are system-level only (php.ini / FPM pool config)
- Zero-config mode: `beacon.enabled=1` + `beacon.service_name="calc"` in FPM pool config
- `ini_set()` cannot change beacon configuration at runtime (by design)

## Related
- `php_beacon.h` — globals struct + BEACON_G macro
- `beacon.c` — ZEND_DECLARE_MODULE_GLOBALS + PHP_INI_BEGIN/END
- `beacon_config.c` — constant registration
