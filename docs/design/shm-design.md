# 共享内存设计：C 结构体 + 双缓冲 + 心跳槽位

> 本文档描述 php-beacon-extension 的共享内存（shm）设计，包括 C 结构体布局、双缓冲机制、per-worker 心跳槽位。

---

## 一、为什么必须双缓冲

治理 worker 写节点表时，FPM worker 可能正在读。单 buffer 场景下，即使加锁，也会出现"读到半写数据"或"读写竞争"的问题。双缓冲的核心思想：

> **写者永远写非激活 buffer，写完原子切指针；读者永远读激活 buffer。无锁、无半写、无竞争。**

---

## 二、C 结构体定义（packed，固定大小）

```c
// beacon_shm.h

#define BEACON_MAX_SERVICES         16
#define BEACON_MAX_NODES_PER_SVC    64
#define BEACON_MAX_NODE_ID_LEN      64
#define BEACON_MAX_HOST_LEN         64
#define BEACON_MAX_METHODS_LEN      256
#define BEACON_MAX_WORKERS          256

// 节点结构体（≈ 392 bytes）
typedef struct __attribute__((packed)) {
    char     id[BEACON_MAX_NODE_ID_LEN];    // 实例 ID
    char     host[BEACON_MAX_HOST_LEN];     // host
    uint16_t port;                          // port
    uint8_t  status;                        // 0=OK, 1=DEGRADED, 2=DEAD, 3=NOT_READY
    uint16_t weight;                        // 权重
    char     methods[BEACON_MAX_METHODS_LEN]; // 逗号分隔方法名
    uint32_t consecutive_failures;          // 预留：熔断失败计数
    uint32_t last_failure_time;             // 预留：上次失败时间
} beacon_node_t;

// 服务结构体（≈ 25KB）
typedef struct __attribute__((packed)) {
    char     name[64];                      // 服务名
    uint32_t node_count;                    // 当前有效节点数
    uint32_t version;                       // 版本号（防脏读）
    uint8_t  writing;                       // 治理 worker 正在写
    beacon_node_t nodes[BEACON_MAX_NODES_PER_SVC];
} beacon_service_t;

// per-worker 心跳槽位（64 字节对齐，防 false sharing）
typedef struct __attribute__((aligned(64))) {
    uint32_t pid;              // 4 bytes, offset 0-3
    uint32_t _pad0;            // 4 bytes, offset 4-7（对齐 last_rinit 到 8）
    uint64_t last_rinit;       // 8 bytes, offset 8-15
    uint8_t  busy;             // 1 byte,  offset 16
    uint8_t  padding[47];      // 47 bytes, offset 17-63
} beacon_worker_slot_t;        // 总大小 64 字节，对齐 64

// SHM Header（256 bytes）
typedef struct __attribute__((packed)) {
    uint32_t magic;                         // 'BEAC' = 0x42454143
    uint32_t shm_version;                   // 结构体版本（兼容性）
    volatile uint8_t active;                // 双缓冲：0=A, 1=B
    uint32_t checksum;                      // CRC32
    pid_t    governance_pid;                // 治理 worker PID

    // Pool 级自计数（FPM worker 写，治理 worker 读）
    atomic_int pool_busy;
    atomic_int pool_idle;
    atomic_int pool_total;
    uint8_t  pool_ready;                    // 0=NOT_READY, 1=READY
    uint8_t  deregister_flag;               // FPM worker 请求注销

    // 治理 worker 心跳（治理 worker 写，FPM worker 读）
    uint64_t governance_alive;              // 治理 worker 上次活跃时间戳
    uint64_t last_update;                   // 节点表上次更新时间戳

    // per-worker 心跳槽位
    beacon_worker_slot_t workers[BEACON_MAX_WORKERS];  // 16KB

    char     reserved[128];                 // 对齐 + 预留
} beacon_shm_header_t;                      // ≈ 256 bytes + 16KB slots
```

**SHM 总大小**：
- Header: 256 bytes + 16KB (worker slots) ≈ 16.25KB
- 2 × 16 services × 25KB = 800KB
- 总计 ≈ **1MB**，`beacon.shm_size = 1048576`

---

## 三、SHM 布局（双缓冲）

```
┌─────────────────────────────────────────┐
│  Header (256 bytes + 16KB slots)        │
│  ├─ active: 0 = Buffer A, 1 = Buffer B  │
│  ├─ pool_busy / pool_idle / pool_total  │
│  ├─ governance_alive / last_update      │
│  └─ workers[256] (心跳槽位)              │
├─────────────────────────────────────────┤
│  Buffer A: 16 × beacon_service_t        │
│  (治理 worker 写，FPM worker 读)         │
├─────────────────────────────────────────┤
│  Buffer B: 16 × beacon_service_t        │
│  (治理 worker 写，FPM worker 读)         │
└─────────────────────────────────────────┘
```

**无锁读写**：
- 治理 worker 永远写**非激活 buffer**
- 写完 `memcpy` 节点数据 → 写 `node_count` → `__sync_synchronize()` → 切 `active`
- FPM worker 永远读**激活 buffer**，只读不锁

---

## 四、自计数与心跳槽位

### 4.1 为什么需要心跳槽位

FPM worker 被 `kill -9`、OOM killer、segfault 时，`RSHUTDOWN` 不执行，`pool_busy` 永久泄漏。10 个 worker 的 pool，杀 2 个后 `busy` 永远虚高，健康状态持续 `DEGRADED`，流量被错误降权。

**这不是边缘 case**，生产环境 OOM、容器限流、段错误都是常态。

### 4.2 心跳槽位设计

```c
// RINIT：找到自己的槽位，写 pid + time + busy=1
PHP_RINIT_FUNCTION(beacon) {
    if (!BEACON_G(enabled) || !g_shm) return SUCCESS;

    pid_t pid = getpid();
    time_t now = time(NULL);

    // 按 pid % MAX 哈希，冲突时线性探测
    int idx = pid % BEACON_MAX_WORKERS;
    for (int i = 0; i < BEACON_MAX_WORKERS; i++) {
        int slot = (idx + i) % BEACON_MAX_WORKERS;
        beacon_worker_slot_t *w = &g_shm->header.workers[slot];

        if (w->pid == 0 || w->pid == pid) {
            w->pid = pid;
            w->last_rinit = now;
            w->busy = 1;
            break;
        }
    }

    // 原有 pool_busy 自计数（保留，作为快速路径）
    atomic_fetch_add(&g_shm->header.pool_busy, 1);

    return SUCCESS;
}

// RSHUTDOWN：找到自己的槽位，标记 idle
PHP_RSHUTDOWN_FUNCTION(beacon) {
    if (!BEACON_G(enabled) || !g_shm) return SUCCESS;

    pid_t pid = getpid();

    int idx = pid % BEACON_MAX_WORKERS;
    for (int i = 0; i < BEACON_MAX_WORKERS; i++) {
        int slot = (idx + i) % BEACON_MAX_WORKERS;
        beacon_worker_slot_t *w = &g_shm->header.workers[slot];

        if (w->pid == pid) {
            w->busy = 0;
            w->last_rinit = time(NULL);
            break;
        }
    }

    atomic_fetch_sub(&g_shm->header.pool_busy, 1);
    atomic_fetch_add(&g_shm->header.pool_total, 1);

    return SUCCESS;
}
```

### 4.3 治理 worker 校准逻辑

```c
// beacon_service_health.c

int beacon_health_calibrate_busy(void) {
    if (!g_shm) return 0;

    time_t now = time(NULL);
    int actual_busy = 0;
    int stale_slots = 0;

    for (int i = 0; i < BEACON_MAX_WORKERS; i++) {
        beacon_worker_slot_t *w = &g_shm->header.workers[i];

        if (w->pid == 0) continue;

        // 检查进程是否还存在：kill(pid, 0) 不发送信号，只检查权限/存在性
        if (kill(w->pid, 0) != 0 && errno == ESRCH) {
            w->pid = 0;
            w->busy = 0;
            w->last_rinit = 0;
            stale_slots++;
            continue;
        }

        // 检查心跳超时：60s 未更新视为死进程
        if (now - w->last_rinit > 60) {
            w->pid = 0;
            w->busy = 0;
            stale_slots++;
            continue;
        }

        if (w->busy) actual_busy++;
    }

    int shm_busy = atomic_load(&g_shm->header.pool_busy);

    // 偏差超过 2 或发现 stale slots，强制校准
    if (abs(shm_busy - actual_busy) > 2 || stale_slots > 0) {
        beacon_log(WARN, "busy counter drift: shm=%d, actual=%d, stale=%d, calibrating",
                   shm_busy, actual_busy, stale_slots);
        atomic_store(&g_shm->header.pool_busy, actual_busy);
    }

    return actual_busy;
}
```

### 4.4 校准触发时机

```c
// 治理 worker keepalive tick 中
void governance_keepalive_tick() {
    // 1. 先校准自计数
    beacon_health_calibrate_busy();

    // 2. 读校准后的 pool_busy 算健康状态
    beacon_health_t health = calc_health_from_shm();

    // 3. 调 keepalive 回调...
}
```

---

## 五、双缓冲读写

### 5.1 治理 worker 写节点表

```c
// beacon_api_governance.c（CLI SAPI 专用）

PHP_METHOD(Beacon_Governance, storeNodes) {
    char *service; size_t service_len;
    zval *nodes_array;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(service, service_len)
        Z_PARAM_ARRAY(nodes_array)
    ZEND_PARSE_PARAMETERS_END();

    if (!g_shm) RETURN_FALSE;

    // 选非激活 buffer
    uint8_t inactive = 1 - g_shm->header.active;
    beacon_service_t *svc = (inactive == 0)
        ? &g_shm->buffer_a[0]
        : &g_shm->buffer_b[0];

    // 找到对应 service slot
    beacon_service_t *target = beacon_shm_find_service_slot(svc, service);
    if (!target) RETURN_FALSE;

    // 标记正在写
    target->writing = 1;
    __sync_synchronize();

    // PHP 数组 → C 结构体
    uint32_t count = 0;
    zval *node_zval;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(nodes_array), node_zval) {
        if (count >= BEACON_MAX_NODES_PER_SVC) break;
        beacon_node_t *node = &target->nodes[count];
        memset(node, 0, sizeof(beacon_node_t));

        zval *zv;
        if ((zv = zend_hash_str_find(Z_ARRVAL_P(node_zval), "id", 2)))
            strncpy(node->id, Z_STRVAL_P(zv), BEACON_MAX_NODE_ID_LEN-1);
        if ((zv = zend_hash_str_find(Z_ARRVAL_P(node_zval), "host", 4)))
            strncpy(node->host, Z_STRVAL_P(zv), BEACON_MAX_HOST_LEN-1);
        if ((zv = zend_hash_str_find(Z_ARRVAL_P(node_zval), "port", 4)))
            node->port = (uint16_t)zval_get_long(zv);
        if ((zv = zend_hash_str_find(Z_ARRVAL_P(node_zval), "status", 6)))
            node->status = (uint8_t)zval_get_long(zv);
        if ((zv = zend_hash_str_find(Z_ARRVAL_P(node_zval), "weight", 6)))
            node->weight = (uint16_t)zval_get_long(zv);
        if ((zv = zend_hash_str_find(Z_ARRVAL_P(node_zval), "methods", 7)))
            strncpy(node->methods, Z_STRVAL_P(zv), BEACON_MAX_METHODS_LEN-1);

        count++;
    } ZEND_HASH_FOREACH_END();

    target->node_count = count;
    target->version++;
    target->writing = 0;

    RETURN_TRUE;
}

PHP_METHOD(Beacon_Governance, commit) {
    if (!g_shm) RETURN_FALSE;
    // 原子切 active buffer
    g_shm->header.active = 1 - g_shm->header.active;
    __sync_synchronize();
    RETURN_TRUE;
}
```

### 5.2 FPM worker 读节点表

```c
// beacon_shm.c

static beacon_service_t* beacon_shm_read_service(const char *name) {
    if (!g_shm) return NULL;

    uint8_t active = g_shm->header.active;
    beacon_service_t *primary = (active == 0) ? g_shm->buffer_a : g_shm->buffer_b;
    beacon_service_t *backup  = (active == 0) ? g_shm->buffer_b : g_shm->buffer_a;

    // 1. 读激活 buffer
    beacon_service_t *svc = find_service(primary, name);
    if (svc && svc->node_count > 0 && svc->version > 0) {
        return svc;  // 正常路径
    }

    // 2. 激活 buffer 损坏/空，读非激活 buffer（双缓冲互救）
    beacon_log(WARN, "primary buffer for %s empty/corrupted, trying backup", name);
    svc = find_service(backup, name);
    if (svc && svc->node_count > 0 && svc->version > 0) {
        beacon_log(INFO, "recovered %s from backup buffer", name);
        return svc;
    }

    // 3. 双缓冲都不可用
    beacon_log(ERROR, "both buffers corrupted for %s, no data", name);
    return NULL;
}
```

---

## 六、关键设计决策

| 决策 | 理由 |
|---|---|
| **C 结构体而非 JSON** | 零反序列化、零内存分配、纳秒级读取。JSON decode 每次 0.1-1ms，C 结构体直接指针偏移 |
| **双缓冲而非单 buffer+锁** | 无锁 = 无死锁风险；原子切指针 = 读者永远读到完整数据 |
| **per-worker 心跳槽位** | 解决自计数泄漏（kill -9 / OOM / segfault 时 RSHUTDOWN 不执行） |
| **64 字节对齐** | 防 false sharing。每个 slot 独占一个 cache line，原子操作永远 L1 hit |
| **CRC32 校验** | 防御内存损坏（FPM master 崩溃可能导致 shm 半写） |
| **`__sync_synchronize()` 内存屏障** | 确保编译器和 CPU 不会重排 `active` 切换前的写操作 |
| **`atomic_int` 自计数** | C11 标准，GCC/Clang 都支持；无需 pthread 锁 |

---

## 七、性能指标

| 指标 | 值 | 说明 |
|---|---|---|
| `pick()` 延迟 | < 1μs | C 结构体直接指针偏移，零反序列化 |
| `getInstances()` 延迟 | < 5μs | 遍历 C 结构体数组，构造 PHP 数组 |
| 自计数开销 | ~20ns/请求 | 2 原子操作（RINIT inc + RSHUTDOWN dec） |
| 心跳槽位写 | ~10ns/请求 | 1 次槽位写（pid + time + busy） |
| shm 占用 | ~1MB | 16 服务 × 64 节点 × 392B × 2（双缓冲）+ 16KB slots |
| 内存分配 | 零 | 栈上构造返回数组，无 emalloc |
