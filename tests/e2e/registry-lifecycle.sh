#!/usr/bin/env bash
# registry-lifecycle.sh — 注册中心生命周期 e2e 实测
#
# 流程：安装扩展 → 启动（注册）→ 查看注册信息 → 睡眠 → 查看注册（keepalive 更新）
#       → 停止（deregister 摘除注册）→ 卸载扩展
#
# 用法：
#   tests/e2e/registry-lifecycle.sh                          # 默认 /var/run/beacon（不可写时自动 sudo -n）
#   BEACON_E2E_REGISTRY_DIR=/tmp/xx tests/e2e/registry-lifecycle.sh  # 免 sudo 自定义路径（file:// 模式）
#
# 环境变量：
#   BEACON_E2E_REGISTRY_DIR  注册中心目录（默认 /var/run/beacon）
#   PHP_BIN                  PHP CLI 路径（默认 PATH 中 php）
#   PHP_CONFIG               php-config 路径（默认 PATH 中 php-config）
#
# 退出码：0 全部通过，1 任一断言失败

set -euo pipefail

cd "$(dirname "$0")/../.."

PHP_BIN="${PHP_BIN:-$(command -v php)}"
PHP_CONFIG="${PHP_CONFIG:-$(command -v php-config)}"
REGISTRY_DIR="${BEACON_E2E_REGISTRY_DIR:-/var/run/beacon}"
SERVICE_NAME="e2e-lifecycle"
REG_FILE="$REGISTRY_DIR/${SERVICE_NAME}.json"
LOG_FILE="$(mktemp -t beacon-e2e)"
MAIN_PID=""

log()  { echo "[e2e] $*"; }
pass() { echo "[e2e] PASS: $*"; }
fail() { echo "[e2e] FAIL: $*"; echo "[e2e] --- main process log ---"; cat "$LOG_FILE"; exit 1; }

cleanup() {
    if [ -n "$MAIN_PID" ]; then
        kill "$MAIN_PID" 2>/dev/null || true
        wait "$MAIN_PID" 2>/dev/null || true
    fi
    rm -f "$LOG_FILE"
}
trap cleanup EXIT

# ---- 1. 构建并安装扩展 ----
log "step 1: build & install extension"
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
# phpize 重跑时需覆盖只读模板文件（首次 phpize 从 PHP 安装目录 cp 来的文件只读）
chmod -R u+w build run-tests.php 2>/dev/null || true
phpize > /dev/null
./configure --enable-beacon > /dev/null
make -j"$JOBS" > /dev/null
make install > /dev/null
pass "extension installed"

# ---- 2. 准备注册中心目录 ----
log "step 2: prepare registry dir: $REGISTRY_DIR"
if ! mkdir -p "$REGISTRY_DIR" 2>/dev/null || [ ! -w "$REGISTRY_DIR" ]; then
    if command -v sudo > /dev/null && sudo -n true 2>/dev/null; then
        sudo -n mkdir -p "$REGISTRY_DIR"
        sudo -n chown "$(id -un)" "$REGISTRY_DIR"
    else
        fail "registry dir $REGISTRY_DIR not writable (set BEACON_E2E_REGISTRY_DIR or configure passwordless sudo)"
    fi
fi
pass "registry dir ready"

# registry_endpoint：默认路径走零配置模式，自定义路径用 file:// 前缀
REGISTRY_ARGS=()
if [ "$REGISTRY_DIR" != "/var/run/beacon" ]; then
    REGISTRY_ARGS=(-d "beacon.registry_endpoint=file://$REGISTRY_DIR")
fi

# ---- 3. 启动主进程（触发注册）----
log "step 3: start main process (service=$SERVICE_NAME)"
"$PHP_BIN" -d extension=beacon.so -d beacon.enabled=1 \
    -d beacon.service_name="$SERVICE_NAME" -d beacon.advertise_port=19001 \
    ${REGISTRY_ARGS[@]+"${REGISTRY_ARGS[@]}"} \
    -r 'sleep(30);' > "$LOG_FILE" 2>&1 &
MAIN_PID=$!

# 等注册文件出现（最多 5s）
for _ in 1 2 3 4 5; do
    [ -f "$REG_FILE" ] && break
    sleep 1
done
[ -f "$REG_FILE" ] || fail "register file not created"
pass "registered: $REG_FILE"

# ---- 4. 查看注册信息 ----
log "step 4: inspect registry content"
cat "$REG_FILE"
grep -q "\"id\": \"${SERVICE_NAME}-" "$REG_FILE" || fail "instance id missing"
grep -q '"port": 19001' "$REG_FILE" || fail "advertise port mismatch"
grep -q '"governance_alive": true' "$REG_FILE" || fail "governance_alive not true"
t1=$(grep -o '"registered_at": [0-9]*' "$REG_FILE" | grep -o '[0-9]*')
pass "registry content valid (registered_at=$t1)"

# ---- 5. 睡眠后查看注册（keepalive 更新）----
log "step 5: sleep 5s (> keepalive_interval 3s), check keepalive update"
sleep 5
t2=$(grep -o '"registered_at": [0-9]*' "$REG_FILE" | grep -o '[0-9]*')
[ "$t2" -gt "$t1" ] || fail "keepalive did not update registered_at ($t1 -> $t2)"
grep -q '"governance_alive": true' "$REG_FILE" || fail "governance_alive turned false (heartbeat lost)"
pass "keepalive updated (registered_at=$t1 -> $t2), governance_alive still true"

# ---- 6. 停止主进程（deregister 摘除注册）----
log "step 6: stop main process, expect deregister"
kill "$MAIN_PID" 2>/dev/null || true
for _ in 1 2 3 4 5; do
    [ ! -f "$REG_FILE" ] && break
    sleep 1
done
[ ! -f "$REG_FILE" ] || fail "deregister did not remove $REG_FILE"
wait "$MAIN_PID" 2>/dev/null || true
MAIN_PID=""
pass "deregistered: registry file removed"

# ---- 7. 卸载扩展 ----
log "step 7: uninstall extension"
EXT_DIR=$("$PHP_BIN" -r 'echo ini_get("extension_dir");')
DATA_DIR="$("$PHP_CONFIG" --prefix)/share/php/beacon"
rm -f "$EXT_DIR/beacon.so"
rm -rf "$DATA_DIR"
[ ! -f "$EXT_DIR/beacon.so" ] || fail "beacon.so not removed"
[ ! -d "$DATA_DIR" ] || fail "data dir not removed"
pass "extension uninstalled (beacon.so + $DATA_DIR removed)"

log "ALL PASS"
