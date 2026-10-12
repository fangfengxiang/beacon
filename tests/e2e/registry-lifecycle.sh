#!/usr/bin/env bash
# Build/install into a private staging root, then verify the real fork+exec
# registration, keepalive, graceful shutdown and staged uninstall lifecycle.
# Optional: PHP_BIN, PHP_CONFIG, BEACON_E2E_REGISTRY_DIR.
set -euo pipefail
cd "$(dirname "$0")/../.."

PHP_BIN="${PHP_BIN:-$(command -v php)}"
PHP_CONFIG="${PHP_CONFIG:-$(command -v php-config)}"
RUNTIME_EXT_DIR=$("$PHP_CONFIG" --extension-dir)
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/beacon-e2e.XXXXXX")
REGISTRY_DIR="${BEACON_E2E_REGISTRY_DIR:-$WORK_DIR/registry}"
SERVICE_NAME="e2e-lifecycle-$$"
REG_FILE="$REGISTRY_DIR/$SERVICE_NAME.json"
LOG_FILE="$WORK_DIR/governance.log"
MAIN_PID=""
INSTALL_DIR="$WORK_DIR/install"
EXT_DIR="$INSTALL_DIR/lib/php/beacon"
DATA_DIR="$INSTALL_DIR/share/php/beacon"

log() { echo "[e2e] $*"; }
pass() { echo "[e2e] PASS: $*"; }
fail() {
    echo "[e2e] FAIL: $*"
    if [ -f "$LOG_FILE" ]; then cat "$LOG_FILE"; fi
    exit 1
}
cleanup() {
    if [ -n "$MAIN_PID" ]; then
        kill "$MAIN_PID" 2>/dev/null || true
        wait "$MAIN_PID" 2>/dev/null || true
    fi
    rm -f "$REG_FILE"
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

log "step 1: build and install into private staging directory"
if [ ! -f Makefile ]; then
    phpize
    ./configure --enable-beacon --with-php-config="$PHP_CONFIG"
fi
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
make -j"$JOBS"
make install INSTALL_ROOT="$INSTALL_DIR" EXTENSION_DIR=/lib/php/beacon BEACON_DATA_DIR=/share/php/beacon
[ -f "$EXT_DIR/beacon.so" ] || fail "staged extension missing"
[ -f "$DATA_DIR/governance.php" ] || fail "staged governance script missing"
pass "extension and governance script installed"

# The exec'd governance PHP uses the same private INI and staged extension_dir.
# Avoid loading the user's installed extensions or changing system php.ini.
export PHPRC="$WORK_DIR/php.ini"
export PHP_INI_SCAN_DIR=""
printf 'extension_dir="%s"\n' "$EXT_DIR" > "$PHPRC"
# pcntl/posix may be shared modules (e.g. official PHP Docker images).
# Load those runtime dependencies by absolute path from the original directory.
for runtime_module in pcntl posix; do
    if [ -f "$RUNTIME_EXT_DIR/$runtime_module.so" ]; then
        printf 'extension="%s/%s.so"\n' "$RUNTIME_EXT_DIR" "$runtime_module" >> "$PHPRC"
    fi
done
unset BEACON_GOVERNANCE_WORKER
"$PHP_BIN" -r 'exit(function_exists("pcntl_async_signals") ? 0 : 1);' || fail "PHP CLI requires pcntl"
mkdir -p "$REGISTRY_DIR"

# A normal PHP exit runs MSHUTDOWN: stop/reap the governance child and remove
# the owned IPC segment. Sending SIGTERM to an unhandled sleep() skips this.
cat > "$WORK_DIR/main.php" <<'PHP'
<?php
pcntl_async_signals(true);
pcntl_signal(SIGTERM, function () { exit(0); });
Beacon::ready();
while (true) { usleep(100000); }
PHP

log "step 2: start parent PHP and real governance worker"
"$PHP_BIN" -d extension=beacon.so -d beacon.enabled=1 \
    -d "beacon.service_name=$SERVICE_NAME" -d beacon.advertise_host=127.0.0.1 \
    -d beacon.advertise_port=19001 -d "beacon.governance_bin=$PHP_BIN" \
    -d "beacon.governance_script=$DATA_DIR/governance.php" \
    -d "beacon.registry_endpoint=file://$REGISTRY_DIR" \
    -d "beacon.shm_key=$((1073741824 + $$))" \
    "$WORK_DIR/main.php" > "$LOG_FILE" 2>&1 &
MAIN_PID=$!

# Read JSON rather than assuming formatting or accepting a partial write.
registry_field() {
    "$PHP_BIN" -r '
        $data = json_decode(@file_get_contents($argv[1]), true);
        foreach (explode(".", $argv[2]) as $key) { $data = $data[$key] ?? null; }
        if ($data === null) exit(1);
        echo is_bool($data) ? ($data ? "true" : "false") : $data;
    ' "$REG_FILE" "$1"
}
for _ in {1..100}; do
    if t1=$(registry_field registered_at 2>/dev/null) && \
       [ "$(registry_field health.governance_alive 2>/dev/null || true)" = true ]; then break; fi
    kill -0 "$MAIN_PID" 2>/dev/null || fail "parent exited before registration"
    sleep 0.1
done
[ -n "${t1:-}" ] || fail "register file not created"
[ "$(registry_field instance.port)" = 19001 ] || fail "advertise port mismatch"
case "$(registry_field instance.id)" in "$SERVICE_NAME"-*) ;; *) fail "instance id mismatch" ;; esac
[ "$(registry_field health.governance_alive)" = true ] || fail "governance heartbeat missing"
pass "registered with valid instance and health"

log "step 3: wait for keepalive timestamp to advance"
for _ in {1..100}; do
    t2=$(registry_field registered_at 2>/dev/null || echo 0)
    [ "$t2" -gt "$t1" ] && break
    sleep 0.1
done
[ "$t2" -gt "$t1" ] || fail "keepalive did not advance ($t1 -> $t2)"
[ "$(registry_field health.governance_alive)" = true ] || fail "governance heartbeat lost"
pass "keepalive updated ($t1 -> $t2)"

log "step 4: gracefully stop parent, verify deregistration"
kill "$MAIN_PID"
wait "$MAIN_PID" || fail "parent shutdown failed"
MAIN_PID=""
[ ! -f "$REG_FILE" ] || fail "deregister did not remove registry file"
pass "governance worker deregistered"

log "step 5: uninstall only the staged artifacts"
rm -f "$EXT_DIR/beacon.so"
rm -rf "$DATA_DIR"
[ ! -f "$EXT_DIR/beacon.so" ] || fail "staged extension not removed"
[ ! -d "$DATA_DIR" ] || fail "staged data directory not removed"
pass "staged extension and script uninstalled"
log "ALL PASS"
