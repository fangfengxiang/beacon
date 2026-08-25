# php-beacon-extension 设计总结

> 状态：设计稿（经 Kimi 技术评审修正）
> 基础：`php-fpm-health-extension.md`（早期健康感知探讨）
> 关联：`lua-resty-php-beacon-plan.md`（beacon 网关）、`lua-resty-php-beacon-etcd-design.md`（etcd 操作）
> 核心命题：把 Swoole 式的自注册基底通过扩展硬塞进 FPM，让 PHP 成为注册中心一等公民

---

## 一、定位

**php-beacon-extension 是 PHP-FPM 的"状态信标与治理调度基底"。**

它解决一个核心矛盾：**FPM 进程是短生命周期的，但服务治理需要长连接和状态持续性感知。**

扩展只做 FPM 做不到的三件事：

1. **感知** —— 自计数 pool 级健康状态（busy/idle/throughput）
2. **调度** —— spawn 常驻治理 worker，维持 timer 循环，驱动 PHP 回调
3. **缓存** —— 双缓冲 shm（C 结构体），让 FPM worker 零 I/O 读取 peer 节点

注册中心通信、健康检查逻辑、服务发现协议，全部交给 PHP 层注入实现。扩展不绑定任何注册中心，不绑定任何协议。

**beacon 的意象**：beacon = 信标 = 主动发光的信号灯。传统探活是"外部用手电筒照 PHP"（beacon 网关发 HTTP 探针猜 PHP 状态），这个扩展是"PHP 自己亮灯"（自计数 → 自报健康 → 通过回调发出去）。

---

## 二、设计原则

### 2.1 扩展 = 状态感知 + 调度基底 + 回调注入

- **状态感知**：RINIT/RSHUTDOWN 自计数 pool 级健康状态（per-worker 心跳槽位，64B 对齐）
- **调度基底**：spawn 常驻治理 worker（fork + close_all_fds + prctl + exec），ReactPHP 协程调度 IO
- **回调注入**：注册中心通信、健康检查逻辑、服务发现协议，全部交给 PHP 层注入实现

### 2.2 每个可变点允许业务层注入 PHP 回调

| 可变点 | 扩展提供 | 业务注入 |
|---|---|---|
| 注册中心通信 | C 层回调调度（`zend_call_function`，无 C 层超时） | PHP 回调实现 register/keepalive/discover/deregister/watch |
| 健康检查算法 | C 层自计数（RINIT/RSHUTDOWN 原子操作 + 心跳槽位校准） | PHP 回调返回业务级健康状态 |
| 节点信息存储 | C 层 shm 双缓冲（C 结构体，无锁读写） | PHP 回调返回节点列表，C 层写 shm |
| 钩子（副作用处理） | C 层回调调度 | PHP 回调实现 on_health_change/on_keepalive_fail 等 |
| 宣告地址解析 | INI 配置 + 自动探测 | PHP 函数返回 {host, port}（云 metadata/多网卡/K8s） |

### 2.3 不绑定注册中心、不绑定协议、不绑定存储

- 注册中心：本地文件（默认）/etcd/Consul/Nacos，由注入决定
- 协议：服务任意 PHP 服务（Yar/gRPC-PHP/HTTP API/Thrift），不绑定 Yar
- 存储：shm（L1）/文件（L2 默认）/Redis/MySQL，由注入决定

### 2.4 配置化是地基

- INI 是唯一配置源，8 个核心项
- 零配置模式：只配 `beacon.enabled = 1` + `beacon.service_name = "calc"` 就能跑
- `bootstrap.php` 只用于代码逻辑注入，不用于配置

### 2.5 极简

- C 扩展 ~8 个文件，核心代码量 < 2000 行
- 无内置重试、无异常类、无 C 层超时
- 请求路径零 I/O（shm 直读，纳秒级）

---

## 三、评估与取舍更新（Kimi 技术评审）

### 3.1 评估总览

Kimi 报告对设计文档进行了逐章技术评审，覆盖 C 扩展工程、FPM 进程模型、微服务治理语义、Go 工程实践、高性能、高可用、SRE 极简主义、调用方/使用方视角等 9 大维度、20+ 个子问题。

**核心结论：报告的技术判断基本正确，且与项目定位高度一致。** 主要修正是将设计从"理论正确但工程危险"拉回到"工程可行且极简"。

### 3.2 关键修正汇总

| 维度 | 原设计 | 修正后 | 评估 |
|------|--------|--------|------|
| 治理 worker 启动 | fork+exec，无 fd 清理 | fork + close_all_fds + prctl + exec | **必须改** |
| 回调超时 | ualarm + zend_try | 删除 C 层超时，PHP 层 ReactPHP 自治 | **必须改** |
| shm 存储格式 | JSON 序列化 | C 结构体数组（packed binary） | **必须改** |
| 自计数泄漏 | 无校准机制 | per-worker 心跳槽位 + kill(pid,0) 校准 | **必须改** |
| 默认注册中心 | etcd | 本地文件（/var/run/beacon/） | **必须改** |
| 配置层级 | INI + configure() + bootstrap | INI 唯一，8 个核心项 | **必须改** |
| 异常体系 | 注册自定义异常类 | 不注册，返回 null/false | **必须改** |
| pull_interval | 5s | 2s | **必须改** |
| False Sharing | 未处理 | slot 对齐到 64 字节 | **必须改** |
| 内置重试 | 未明确 | 坚决不提供，独立 beacon-yar 包 | **必须改** |

### 3.3 不采纳的建议

| 建议 | 不采纳理由 |
|------|-----------|
| C 层内置 libcurl multi 做异步 IO | 引入 libcurl 依赖，构建复杂。ReactPHP 已解决异步问题，C 层保持极简 |
| 治理 worker 纯 C 实现（不 exec PHP） | 违背 SPI 注入的核心设计原则。没有 VM 就不能跑 PHP 回调，扩展失去灵活性 |
| 一期内置 Consul 客户端 | 绑定注册中心，违背"不绑定注册中心"的核心设计原则 |
| FPM master 主循环支持 UNIX socket IPC | FPM master 主循环是封闭的（阻塞在 waitpid），扩展无法修改 |
| 扩展 fork 子进程异步回调 PHP 函数 | PHP VM 不是 fork-safe 的，子进程继承 VM 状态后不能再初始化 |

---

## 四、架构：治理 worker 进程

### 4.1 为什么需要 worker 进程

FPM 的硬约束：**后台保活/定时器跑在 master，master 线程没有 VM，不能调 PHP userland**。要"允许注入 PHP 函数"，后台执行体必须有 VM。

解法：扩展从 FPM master **fork 一个治理 worker 进程**——它是 fork 出来的，加载了扩展 + autoloader，**带 VM**，能调 PHP userland。对标 Swoole 的 `Swoole\Process`。

```
FPM master（长生命周期）
  ├─ worker 1..N（请求级，请求结束销毁，无后台能力）
  └─ governance worker（fork 自 master，带 VM，常驻）
       ├─ keepalive timer → 调注入的 registry.keepalive + health.check
       ├─ pull timer       → 调注入的 registry.discover → persistence.store
       └─ watch loop       → 调注入的 registry.watch → persistence.store on change
```

### 4.2 worker 生命周期

- **启动**：FPM pool init 时 fork，调注入的 `on_start` 钩子（注册 self、起 timer）
- **运行**：循环跑 keepalive/pull/watch，调注入的 PHP 函数
- **崩溃**：master 监控，崩溃重启（重启后重新注册，幂等）
- **停止**：FPM pool stop 时调注入的 `on_stop` 钩子（deregister），kill worker

### 4.3 IPC：worker → FPM worker

FPM worker 调 `Beacon::getInstances($service)` 时，不直接调治理 worker（跨进程调用慢）。治理 worker 把节点表写进 **sysv shm**，FPM worker 从 shm 读（共享内存，快）。

```
governance worker ──写──> sysv shm（节点表，C 结构体）<──读── FPM worker
```

shm 是默认持久化存储；若业务注入了别的持久化（如 Redis），shm 降级为 L1 缓存。

### 4.4 治理 worker 的部署方案

**方案选择：FPM spawn（保留进程关系）**

| 方案 | 做法 | 优点 | 缺点 | 推荐 |
|---|---|---|---|---|
| **A：FPM master fork + exec** | 治理 worker 是 FPM master fork 的子进程，exec PHP 脚本 | 扩展自带，零部署；进程关系带来 prctl/SIGCHLD 能力 | 需要 fd 清理 | ✅ 推荐 |
| **B：systemd 独立服务** | 治理 worker 是独立 PHP 脚本，由 systemd 启动 | 天然独立，不受 FPM 进程树影响 | 需要额外部署；失去进程关系 | 二期可选 |

**为什么方案 A 推荐**：

1. **进程关系价值**：`prctl(PR_SET_PDEATHSIG, SIGTERM)` 让内核在 FPM master 退出时自动通知治理 worker，解决 graceful reload 孤儿问题
2. **SIGCHLD 精准监控**：FPM master 可以精准感知治理 worker 崩溃，毫秒级重启
3. **开箱即用**：装扩展即用，不需要配 systemd

### 4.5 fd 清理（必须）

`exec()` 替换的是进程的地址空间（代码段、数据段、堆、栈），但**文件描述符表是内核维护的进程级资源**，`exec()` 不会动它。除非 fd 事先被标记了 `FD_CLOEXEC`，否则子进程会完整继承 FPM master 在 MINIT 阶段已打开的所有 fd。

**防御方案**（在子进程 exec 前执行）：

```c
static void close_all_fds_except_std(void) {
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) return;

    int dir_fd = dirfd(dir);
    struct dirent *entry;
    int fds[1024];
    int count = 0;

    while ((entry = readdir(dir)) != NULL && count < 1024) {
        int fd = atoi(entry->d_name);
        if (fd > 2 && fd != dir_fd) {  // 保留 0/1/2，保留 opendir 自己的 fd
            fds[count++] = fd;
        }
    }
    closedir(dir);

    for (int i = 0; i < count; i++) {
        close(fds[i]);
    }
}
```

### 4.6 完整 spawn 实现

```c
pid_t beacon_spawn_governance_worker() {
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        // 子进程：清理 fd → 脱离会话 → 设置父进程死亡信号 → exec
        close_all_fds_except_std();
        setsid();
        prctl(PR_SET_PDEATHSIG, SIGTERM);

        execl(BEACON_PHP_BIN, "php", governance_script, NULL);
        _exit(127);
    }

    // 父进程（FPM master）：记录 pid 到 shm
    beacon_shm_set_governance_pid(pid);
    return pid;
}
```

---

## 五、运行原理与数据流

### 5.1 进程架构

```
┌─────────────────────────────────────────────────────────────┐
│  FPM master（长生命周期，唯一常驻）                           │
│                                                              │
│  MINIT: 解析 INI → 初始化 shm → spawn 治理 worker           │
│  （fork + close_all_fds + prctl + exec governance.php）      │
│  监控: 从 shm 读 governance_pid，kill(pid,0) 检测，崩溃重启  │
│                                                              │
│  ┌────────────────────────┐   ┌──────────────────────────┐  │
│  │ governance worker      │   │ FPM worker 1..N          │  │
│  │ (独立进程，带 VM)       │   │ (请求级，请求结束销毁)    │  │
│  │                        │   │                          │  │
│  │ ReactPHP event loop    │   │ RINIT: shm atomic inc   │  │
│  │   keepalive timer ─────┼─→ │   (busy++)  ← 1 原子操作  │  │
│  │   discover timer ──────┼─→ │   + 心跳槽位写 pid+time  │  │
│  │   调 PHP 回调           │   │ ... 处理请求 ...          │  │
│  │   写 shm（C 结构体）    │   │   Beacon::pick() ← shm 读│  │
│  │                        │   │   new Yar_Client(...)     │  │
│  │                        │   │   $client->method()       │  │
│  │                        │   │                          │  │
│  │                        │   │ RSHUTDOWN: shm atomic dec│  │
│  │                        │   │   (busy--)  ← 1 原子操作  │  │
│  └────────────────────────┘   └──────────────────────────┘  │
│                  ↑                          ↑                 │
│                  └──── sysv shm（C 结构体 + 心跳槽位）────────┘
│                  写（治理 worker）    读（FPM worker 取节点 / 读自计数）
└─────────────────────────────────────────────────────────────┘
                          ↕
                   ┌──────┴──────┐
                   │  注册中心    │  ← 由 PHP 回调注入决定（文件/etcd/Consul）
                   │ (文件/etcd)  │
                   └─────────────┘
```

### 5.2 数据流

**启动流**：
```
FPM master start
  → MINIT: 解析 INI（service_name, advertise, registry_endpoint）→ 初始化 shm
  → spawn 治理 worker（fork + close_all_fds + prctl + exec governance.php）
     → 治理 worker: 初始化 VM → 执行 governance.php（ReactPHP event loop）
     → 治理 worker: 调 on_register 回调（异步投递，不等待结果）
     → 治理 worker: 起 keepalive timer / pull timer
     → 治理 worker: 调 on_discover 回调 → 写 shm（同步）
     → 治理 worker: 首次 keepalive 成功 或 收到 Beacon::ready() → 状态转 Beacon::HEALTH_OK
  → fork FPM workers（继承 shm，不继承注入对象——worker 不需要注入对象）
```

**稳态后台流（治理 worker，脱离请求路径）**：
```
每 keepalive_interval(3s):
  health = 读 shm 自计数器（busy/idle/throughput）→ 算 pool 级健康
  调 on_keepalive 回调（ReactPHP 协程，异步 IO，不阻塞其他 timer）

每 pull_interval(2s):
  nodes = 调 on_discover 回调（ReactPHP 协程，异步 IO）
  写 shm（C 结构体，同步，FPM worker 立即可读）
```

**请求路径流（FPM worker，无后台工作）**：
```
RINIT:    shm atomic inc(busy) + 心跳槽位写 pid+time+busy=1
... 处理请求 ...
  若需调 peer:
    $node = Beacon::pick('user')         -- 读 shm C 结构体 + 进程内 LB，无 I/O
    $client = new Yar_Client($node)      -- PHP 创建 client
    $result = $client->method($args)      -- RPC 到 peer
    失败 → pick(exclude=tried) 重试
RSHUTDOWN: shm atomic dec(busy) + 心跳槽位 busy=0
```

**停止流**：
```
FPM pool stop
  → FPM master: kill(governance_pid, SIGTERM)
  → 治理 worker: 收到 SIGTERM → 调 on_deregister 回调（异步投递，等 2s 让请求飞出去）→ 退出
```

### 5.3 为什么不干扰 worker

**请求路径只碰 shm，不碰网络、不跑 timer、不阻塞**：

| 请求内动作 | 开销 | 频率 |
|---|---|---|
| RINIT `shm inc(busy)` + 心跳槽位写 | 2 原子内存操作（~20ns） | 每请求 1 次 |
| RSHUTDOWN `shm dec(busy)` + 心跳槽位写 | 2 原子内存操作（~20ns） | 每请求 1 次 |
| `Beacon::pick()` | shm 读（C 结构体，零反序列化）+ 进程内 LB | 按需（调 peer 时） |

10k req/s 下自计数总开销 = 40k 原子操作/s ≈ 0.4ms/s，**可忽略**。worker 100% 时间服务请求，零后台工作。

### 5.4 为什么高稳定

| 故障 | 扩展行为 | 兜底 |
|---|---|---|
| 治理 worker 崩溃 | master 从 shm 读 `governance_pid`，`kill(pid, 0)` 检测 → 重启 → 重新 register（幂等） | 重启窗口内 etcd lease TTL(15s) 保活，不误摘 |
| 注入的 PHP 实现抛异常 | C 层 `zend_call_function` 包 try → 降级内置默认 | 不崩 worker，keepalive 不中断 |
| shm 损坏 | 双缓冲互救：激活 buffer 坏 → 读备份 buffer | 文件模式恢复：治理 worker 重启后从文件恢复 shm |
| etcd 不可达 | keepalive 失败累积 → lease 过期被摘 | beacon 侧 L2 TTL / 故障转移兜底 |
| FPM master 崩溃 | FPM 本身宕（扩展管不了） | beacon 探活/lease 过期检测，摘除该实例 |

**关键隔离**：治理 worker 和 FPM worker 是**独立进程**——worker 崩不影响治理（治理继续保活），治理崩不影响 worker（worker 仍服务请求，只是节点缓存陈旧直到 worker 重启）。

---

## 六、共享内存设计：C 结构体 + 双缓冲 + 心跳槽位

### 6.1 为什么必须双缓冲

治理 worker 写节点表时，FPM worker 可能正在读。单 buffer 场景下，即使加锁，也会出现"读到半写数据"或"读写竞争"的问题。双缓冲的核心思想：

> **写者永远写非激活 buffer，写完原子切指针；读者永远读激活 buffer。无锁、无半写、无竞争。**

### 6.2 C 结构体定义（packed，固定大小）

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

### 6.3 SHM 布局（双缓冲）

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

### 6.4 自计数与心跳槽位

**为什么需要心跳槽位**：FPM worker 被 `kill -9`、OOM killer、segfault 时，`RSHUTDOWN` 不执行，`pool_busy` 永久泄漏。10 个 worker 的 pool，杀 2 个后 `busy` 永远虚高，健康状态持续 `DEGRADED`，流量被错误降权。

**心跳槽位设计**：

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

**治理 worker 校准逻辑**：

```c
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

### 6.5 双缓冲读写

**治理 worker 写节点表**：

```c
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

**FPM worker 读节点表**：

```c
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

### 6.6 关键设计决策

| 决策 | 理由 |
|---|---|
| **C 结构体而非 JSON** | 零反序列化、零内存分配、纳秒级读取。JSON decode 每次 0.1-1ms，C 结构体直接指针偏移 |
| **双缓冲而非单 buffer+锁** | 无锁 = 无死锁风险；原子切指针 = 读者永远读到完整数据 |
| **per-worker 心跳槽位** | 解决自计数泄漏（kill -9 / OOM / segfault 时 RSHUTDOWN 不执行） |
| **64 字节对齐** | 防 false sharing。每个 slot 独占一个 cache line，原子操作永远 L1 hit |
| **CRC32 校验** | 防御内存损坏（FPM master 崩溃可能导致 shm 半写） |
| **`__sync_synchronize()` 内存屏障** | 确保编译器和 CPU 不会重排 `active` 切换前的写操作 |
| **`atomic_int` 自计数** | C11 标准，GCC/Clang 都支持；无需 pthread 锁 |

### 6.7 性能指标

| 指标 | 值 | 说明 |
|---|---|---|
| `pick()` 延迟 | < 1μs | C 结构体直接指针偏移，零反序列化 |
| `getInstances()` 延迟 | < 5μs | 遍历 C 结构体数组，构造 PHP 数组 |
| 自计数开销 | ~20ns/请求 | 2 原子操作（RINIT inc + RSHUTDOWN dec） |
| 心跳槽位写 | ~10ns/请求 | 1 次槽位写（pid + time + busy） |
| shm 占用 | ~1MB | 16 服务 × 64 节点 × 392B × 2（双缓冲）+ 16KB slots |
| 内存分配 | 零 | 栈上构造返回数组，无 emalloc |

---

## 七、治理 Worker：生命周期、spawn、状态机

### 7.1 治理 worker 是什么

治理 worker 是一个**独立的 PHP CLI 进程**，由 FPM master 在 MINIT 阶段 spawn（fork + exec）。它：

- 有完整的 PHP VM，能执行 PHP 回调（注册中心通信、健康检查、服务发现）
- 运行 ReactPHP event loop，协程调度 IO，不阻塞 timer
- 通过 shm 和 FPM worker 通信（写节点表、读自计数）
- 崩溃后由 FPM master 检测并重启

### 7.2 spawn 机制

**启动流程**：

```c
pid_t beacon_spawn_governance_worker() {
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        // 子进程：清理 fd → 脱离会话 → 设置父进程死亡信号 → exec
        close_all_fds_except_std();
        setsid();
        prctl(PR_SET_PDEATHSIG, SIGTERM);

        execl(BEACON_PHP_BIN, "php", governance_script, NULL);
        _exit(127);
    }

    // 父进程（FPM master）：记录 pid 到 shm
    beacon_shm_set_governance_pid(pid);
    return pid;
}
```

**关键机制**：

| 机制 | 作用 | 为什么必须 |
|------|------|-----------|
| `close_all_fds_except_std()` | 关闭所有非标准 fd | 防止继承 FPM 的 listen socket、数据库连接等 |
| `setsid()` | 脱离控制终端 | 避免 FPM 收到 SIGHUP 时传导给治理 worker |
| `prctl(PR_SET_PDEATHSIG, SIGTERM)` | 父进程退出时内核自动发 SIGTERM | 解决 graceful reload 时旧治理 worker 孤儿化问题 |
| `exec()` | 替换地址空间，启动全新 VM | fork 后 VM 状态已污染，必须 exec 重置 |

**exec 路径探测**：

```c
static const char* beacon_get_php_bin(void) {
    // 运行时 INI 优先
    if (BEACON_G(governance_bin) && strlen(BEACON_G(governance_bin)) > 0) {
        return BEACON_G(governance_bin);
    }
    // 编译时宏兜底
    return BEACON_PHP_BIN;
}

static const char* beacon_get_governance_script(void) {
    // 运行时 INI 优先
    if (BEACON_G(governance_script) && strlen(BEACON_G(governance_script)) > 0) {
        return BEACON_G(governance_script);
    }
    // 内置默认脚本路径
    return BEACON_DATA_DIR "/governance.php";
}
```

### 7.3 生命周期状态机

**状态定义**：

```
┌─────────────────────────────────────────────────────────────┐
│                    治理 Worker 内部状态                       │
├─────────────────────────────────────────────────────────────┤
│  GW_INIT        刚 spawn，初始化 VM 和扩展                    │
│  GW_BOOTSTRAP   执行 governance.php，注入回调                 │
│  GW_REGISTERING 调 on_register 回调（异步投递）               │
│  GW_STEADY      主循环：keepalive + discover 定时轮询          │
│  GW_DEREGISTERING 收到 SIGTERM，调 on_deregister（异步投递）   │
│  GW_EXIT        清理资源，退出进程                             │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                    FPM Master 视角状态                      │
├─────────────────────────────────────────────────────────────┤
│  MASTER_MONITORING  正常运行，定期 kill(pid,0) 检测           │
│  MASTER_RESTARTING  检测到 worker 死亡，正在重新 spawn         │
│  MASTER_DEGRADED    连续 5 次重启失败，停止重启，降级运行      │
└─────────────────────────────────────────────────────────────┘
```

**状态转换图**：

```
                        FPM MASTER
                           │
                           ▼ MINIT spawn()
                  ┌────────┴────────┐
                  │    GW_INIT      │
                  │  zend_startup() │
                  └────────┬────────┘
                           │
                           ▼
                  ┌────────┴────────┐
                  │  GW_BOOTSTRAP   │
                  │ exec governance │
                  │ validate SPI    │
                  └────────┬────────┘
                           │
              ┌────────────┼────────────┐
              │ 回调完整    │ 回调缺失/异常 │
              ▼            │            ▼
       ┌──────┴──────┐     │     ┌──────┴──────┐
       │GW_REGISTERING│     │     │   GW_EXIT   │
       │  async fire   │     │     │  log fatal  │
       └──────┬──────┘     │     └─────────────┘
              │            │
              ▼ (不等待结果)
       ┌──────┴──────┐
       │   GW_STEADY  │◄────────────────────────┐
       │  main loop   │                         │
       └──────┬──────┘                         │
              │                                │
       ┌──────┴──────┐   ┌──────────────┐     │
       │ keepalive   │   │  discover    │     │
       │ tick (3s)   │   │  tick (2s)   │     │
       │ async fire  │   │ sync call    │     │
       └──────┬──────┘   │ write shm    │     │
              │           └──────┬──────┘     │
              │                  │             │
              └──────────────────┴─────────────┘
                                     │
                        SIGTERM      │
                           │         │
                           ▼         │
                  ┌────────┴────────┐│
                  │GW_DEREGISTERING ││
                  │  async fire     ││
                  └────────┬────────┘│
                           │         │
                           ▼         │
                  ┌────────┴────────┐│
                  │    GW_EXIT      ││
                  │  cleanup        ││
                  └─────────────────┘│
                                     │
       ┌─────────────────────────────┘
       │
       │  FPM Master 视角
       ▼
    ┌──┴──────────────┐
    │ MASTER_MONITORING │◄────────────────────────┐
    │ kill(pid,0) ok   │                         │
    └──┬──────────────┘                         │
       │                                         │
       │ kill(pid,0) fail                        │
       ▼                                         │
    ┌──┴──────────────┐   重启成功               │
    │MASTER_RESTARTING│─────────────────────────┘
    │  spawn() again  │
    └──┬──────────────┘
       │ 连续 5 次失败
       ▼
    ┌──┴──────────────┐
    │ MASTER_DEGRADED │
    │ stop restarting │
    └─────────────────┘
```

### 7.4 治理 worker 主循环（ReactPHP）

治理 worker 是 `php governance.php`，它运行 ReactPHP event loop：

```php
<?php
// /usr/share/php/beacon/governance.php（内置默认脚本）

use React\EventLoop\Loop;
use React\Http\Browser;

$loop = Loop::get();
$browser = new Browser($loop);

// 从环境变量或命令行参数读取配置
$pool = $argv[array_search('--pool', $argv) + 1] ?? 'www';
$shmKey = $argv[array_search('--shm-key', $argv) + 1] ?? null;

// 默认使用 FileRegistry
$registry = new \Beacon\Adapter\FileRegistry('/var/run/beacon');

// ========== keepalive timer（3s）==========
$loop->addPeriodicTimer(3.0, function () use ($registry) {
    $health = calc_health_from_shm();  // C 扩展 API，读 shm 自计数
    $registry->keepalive($health);
});

// ========== discover timer（2s）==========
$loop->addPeriodicTimer(2.0, function () use ($registry) {
    $nodes = $registry->discover('calc');
    // C 扩展 API：PHP 数组 → C 结构体 → 写 shm
    Beacon\Governance::storeNodes('calc', $nodes);
    Beacon\Governance::commit();
});

$loop->run();
```

**关键设计**：
- ReactPHP 的 `Browser->post()` 返回 Promise，HTTP IO 在 event loop 中异步调度
- 一个请求超时只是那个 Promise reject，不影响其他 timer
- C 扩展只提供同步 API（`storeNodes()` / `commit()`），不碰网络

### 7.5 治理 worker 崩溃与重启

**FPM master 监控**：

```c
PHP_MINIT_FUNCTION(beacon) {
    // ... 初始化 shm ...

    // 检查 shm 中是否已有治理 worker pid
    pid_t old_pid = beacon_shm_get_governance_pid();
    if (old_pid > 0 && kill(old_pid, 0) == 0) {
        // 旧治理 worker 还在跑（prctl 还没触发或延迟）
        // 等 500ms 让它自然退出，避免立即 spawn 双实例
        usleep(500000);
    }

    // spawn 新治理 worker
    beacon_spawn_governance_worker();

    return SUCCESS;
}
```

**治理 worker 自检（getppid 兜底）**：

```c
// 治理 worker 主循环中，每 10s 检测一次
pid_t original_ppid = getppid();

while (g_worker.running) {
    // ... keepalive / discover ...

    if (++tick % 100 == 0) {  // 100ms * 100 = 10s
        pid_t current_ppid = getppid();
        if (current_ppid != original_ppid && original_ppid != 1) {
            beacon_log(WARN, "parent changed %d -> %d, exiting",
                      original_ppid, current_ppid);
            g_worker.running = 0;
            g_worker.state = GW_DEREGISTERING;
            break;
        }
    }

    usleep(100000);  // 100ms
}
```

**优雅退出**：

```c
// 治理 worker 信号处理
static void gw_sigterm_handler(int sig) {
    beacon_log(INFO, "received SIGTERM (parent exited or reload), deregistering");
    g_worker.running = 0;
    g_worker.state = GW_DEREGISTERING;
}

// 主循环退出前
if (g_worker.state == GW_DEREGISTERING) {
    beacon_log(INFO, "deregistering service: %s", BEACON_G(service_name));
    gw_async_callback(BEACON_CB_DEREGISTER, NULL);
    usleep(2000000);  // 2s 让请求飞出去
}
```

### 7.6 无治理模式

**定义**：治理 worker 反复崩溃（连续 5 次重启失败）→ 进入"无治理模式"：

| 状态 | 触发条件 | FPM 行为 | 治理行为 |
|------|---------|---------|---------|
| FULL | 治理 worker 正常 | pick() 读 shm，正常服务 | 注册/保活/发现 |
| DEGRADED | 治理 worker 崩溃，但 shm 有效 | pick() 读 shm，标记陈旧 | 无，等重启 |
| NONE | 治理 worker 反复崩溃，或 shm 损坏 | pick() 返回空 / 异常 | 无 |

**缓存 TTL**：

```c
// pick() 的降级逻辑
int beacon_pick_with_fallback(const char *service, zval *return_value) {
    beacon_service_t *svc = beacon_shm_read_service(service);

    if (!svc) {
        ZVAL_NULL(return_value);
        return BEACON_MODE_NONE;
    }

    time_t now = time(NULL);
    int stale = (int)(now - g_shm->header.last_update);

    if (stale > 60 && stale <= 300) {
        // 1-5 分钟陈旧：DEGRADED，仍返回但告警
        add_assoc_bool(return_value, "_stale", 1);
        return BEACON_MODE_DEGRADED;
    }

    if (stale > 300) {
        // > 5 分钟：视为无效，返回 null
        ZVAL_NULL(return_value);
        return BEACON_MODE_NONE;
    }

    // 正常
    return BEACON_MODE_FULL;
}
```

**SRE 可观测性**：

```c
// Beacon::status() —— 新增 API，供 health check endpoint 调用
PHP_METHOD(Beacon, status) {
    array_init(return_value);

    add_assoc_string(return_value, "pool", BEACON_G(pool_name));
    add_assoc_long(return_value, "pool_busy", atomic_load(&g_shm->header.pool_busy));
    add_assoc_long(return_value, "pool_idle", atomic_load(&g_shm->header.pool_idle));

    pid_t gov_pid = g_shm->header.governance_pid;
    add_assoc_long(return_value, "governance_pid", gov_pid);

    if (gov_pid > 0 && kill(gov_pid, 0) == 0) {
        add_assoc_string(return_value, "governance_status", "running");
    } else {
        add_assoc_string(return_value, "governance_status", "down");
    }

    time_t last_update = g_shm->header.last_update;
    time_t now = time(NULL);
    add_assoc_long(return_value, "cache_age_seconds", (long)(now - last_update));

    if (now - last_update > 60) {
        add_assoc_string(return_value, "mode", "degraded");
    } else {
        add_assoc_string(return_value, "mode", "full");
    }
}
```

### 7.7 关键设计决策

| 决策 | 理由 |
|------|------|
| **FPM spawn 而非 systemd** | 进程关系带来 prctl/SIGCHLD 能力，开箱即用 |
| **fork + exec 而非纯 fork** | PHP VM 单例限制，fork 后必须 exec 重置 VM |
| **ReactPHP 而非 C 层 libcurl** | C 层保持极简，PHP 层自治超时，不引入 libcurl 依赖 |
| **prctl + getppid 双保险** | 内核级 + 用户态兜底，解决 graceful reload 孤儿问题 |
| **无治理模式语义** | 缓存 TTL + Beacon::status()，SRE 可观测 |

---

## 八、SPI：PHP 回调注入

### 8.1 核心变化

SPI 从 C 层接口改为 PHP 回调。C 层只做回调调度（`zend_call_function` + `zend_try`），不内置任何注册中心实现。

**关键约束**：
- C 层不做超时（ualarm 已删除，信号不安全）
- PHP 层自治超时（ReactPHP 协程，HTTP 客户端自带 `CURLOPT_TIMEOUT_MS`）
- C 层只做"善意提示"：记录回调耗时，超阈值记 warn，但不中断

### 8.2 注册中心回调（Registry Callbacks）

扩展不内置任何注册中心协议，全部通过 PHP 回调注入：

```php
// 服务注册（pool 启动时调一次）
Beacon::setOpt(Beacon::OPT_ON_REGISTER, function(array $ctx) {
    // $ctx = ['service' => 'calc', 'instance' => [...], 'health' => [...]]
    // 用户自己决定连什么注册中心
});

// 保活（每 keepalive_interval 调一次）
Beacon::setOpt(Beacon::OPT_ON_KEEPALIVE, function(array $ctx) {
    // $ctx = ['service' => 'calc', 'instance' => [...], 'health' => [...]]
});

// 服务注销（pool 停止时调一次）
Beacon::setOpt(Beacon::OPT_ON_DEREGISTER, function(array $ctx) {
    // $ctx = ['service' => 'calc', 'instance' => [...]]
});

// 节点发现（每 pull_interval 调一次，必须返回节点数组）
Beacon::setOpt(Beacon::OPT_ON_DISCOVER, function(string $service): array {
    // 返回节点列表，C 扩展写 shm
    return [['id' => '...', 'host' => '...', 'port' => ..., 'status' => ..., 'methods' => [...]]];
});

// 节点监听（可选，长驻）
Beacon::setOpt(Beacon::OPT_ON_WATCH, function(string $service, callable $onChange) {
    // 有变更时调 $onChange($nodes) 通知 C 扩展写 shm
});
```

**回调分级**：

| 回调 | 是否必须 | 调用方式 | 说明 |
|---|---|---|---|
| `on_register` | 软必须 | 异步 | 启动时调一次。缺失时记 warn，扩展仍启动 |
| `on_keepalive` | **硬必须** | 异步 | 每 keepalive_interval 调。缺失时扩展报错退出 |
| `on_discover` | **硬必须** | 同步 | 每 pull_interval 调，必须返回节点数组。缺失时扩展报错退出 |
| `on_deregister` | 软必须 | 异步 | pool 停止时调一次。缺失时记 warn |
| `on_watch` | 可选 | 长驻 | 有则启用推送，无则靠 discover 轮询兜底 |

**内置默认：FileRegistry**

扩展自带 `governance.php`，内置 `FileRegistry`（本地文件注册中心）：

```php
<?php
// /usr/share/php/beacon/governance.php（内置默认脚本）

namespace Beacon\Adapter;

class FileRegistry {
    private string $dataDir;

    public function __construct(string $dataDir = '/var/run/beacon') {
        $this->dataDir = $dataDir;
        if (!is_dir($dataDir)) {
            mkdir($dataDir, 0755, true);
        }
    }

    public function register(array $ctx): void {
        $service = $ctx['service'];
        $file = "{$this->dataDir}/{$service}.json";

        $data = [
            'instance' => $ctx['instance'],
            'health' => $ctx['health'] ?? ['status' => 'not_ready'],
            'registered_at' => time(),
        ];

        file_put_contents($file, json_encode($data, JSON_PRETTY_PRINT));
    }

    public function keepalive(array $ctx): void {
        $this->register($ctx);  // 直接覆盖写
    }

    public function deregister(array $ctx): void {
        $service = $ctx['service'];
        $file = "{$this->dataDir}/{$service}.json";
        if (file_exists($file)) {
            unlink($file);
        }
    }

    public function discover(string $service): array {
        $file = "{$this->dataDir}/{$service}.json";
        if (!file_exists($file)) {
            return [];
        }

        $data = json_decode(file_get_contents($file), true);
        return [$data['instance'] ?? []];
    }

    public function watch(string $service, callable $onChange): void {
        // 文件模式无 watch，靠轮询
    }
}
```

**文件模式的优势**：
- 零依赖（不需要部署 etcd 集群）
- 调试成本极低（`tail -f /var/run/beacon/calc.json`）
- 天然 L2 持久化（治理 worker 重启后从文件恢复 shm）
- 后续升级 etcd 只需换适配器，接口不变

### 8.3 健康检查器（HealthChecker）

```
interface HealthChecker {
  check(): array;   -- 返回 {status: Beacon::HEALTH_OK|HEALTH_DEGRADED|HEALTH_DEAD, metrics: {...}}
}
```

- **内置默认**：`FpmPoolHealthChecker`——master 视角自感知（RINIT/RSHUTDOWN 自计数 busy/idle + shm 汇总），报 FPM 池级健康
- **注入**：业务实现此接口，报业务级健康（"DB 连上了吗？缓存热了吗？依赖服务通吗？"）
- **keepalive 携带**：`keepalive($id, $health)` 把健康数据带去注册中心，注册中心/store 的健康是**自报**不是探活

**链式注册 + 最差聚合**：

`registerHealthChecker()` 是**追加**不是覆盖——可注册多个 HealthChecker，链式执行，结果取最差状态。内置 `FpmPoolHealthChecker` 默认已注册（报进程级 busy/saturation），业务注入的是第二个（报依赖级 DB/缓存），两者组合。

聚合规则（最差优先）：

```
NOT_READY > DEAD > DEGRADED > OK

任一 NOT_READY → 整体 NOT_READY（启动中，不接流量）
任一 DEAD      → 整体 DEAD（摘除）
任一 DEGRADED  → 整体 DEGRADED（降优先级）
全部 OK        → 整体 OK（接全量流量）
```

**三态对标业界**：

| beacon 三态 | gRPC Health | K8s probe | Consul | Envoy | 语义 |
|---|---|---|---|---|---|
| `HEALTH_OK` | SERVING | passing | passing | healthy | 可接全量流量 |
| `HEALTH_DEGRADED` | — | warning | warning | degraded（降优先级） | 饱和/降级，接少量流量 |
| `HEALTH_DEAD` | NOT_SERVING | critical | critical | unhealthy/ejected | 摘除，不接流量 |

### 8.4 持久化存储（Persistence）

```
interface Persistence {
  store(string $service, array $nodes): bool;   -- 存节点表
  load(string $service): array;                 -- 读节点表
  invalidate(string $service): void;            -- 失效（watch 触发）
}
```

- **内置默认**：`ShmPersistence`（sysv shm，进程内，快）
- **注入**：业务实现，存到任意位置——Redis（跨进程共享）、MySQL、文件、甚至写回注册中心本身
- **用途**：治理 worker `discover`/`watch` 后 `store`，FPM worker `getInstances` 时 `load`（或直接读 shm）

**定位澄清：Persistence 是 L2 持久化（可选），不是 L1 缓存**

```
shm = L1 缓存（必须，FPM worker 只读这里，纳秒级）
Persistence = L2 持久化（可选，治理 worker 写，pool 重启后恢复用）
```

- **FPM worker 永远只读 shm**，不读 Persistence
- **治理 worker 写 shm + 异步写 Persistence**（如果注入了）
- **pool 重启后**：治理 worker 先从 Persistence 恢复 shm（快速），再从注册中心拉最新（权威）

**一期不实现 Persistence SPI**——shm 作为唯一存储（L1），pool 重启后从注册中心重新拉取（几秒延迟可接受）。Persistence SPI 保留接口定义，后续按需实现。

### 8.5 钩子（Hooks）

```
interface Hooks {
  -- 必须（核心功能，业务需要感知）
  on_health_change(string $service, int $old_status, int $new_status, array $metrics): void;
  on_keepalive_fail(string $service, int $consecutive_failures): void;
  on_instances_changed(string $service, array $added, array $removed, array $updated): void;

  -- 可选（增强可观测性，不注入无影响）
  on_register(string $service, array $instance): void;
  on_deregister(string $service, string $id): void;
  on_governance_crash(string $service, string $reason): void;
}
```

- **内置默认**：空实现（不产生副作用）
- **注入**：业务实现此接口，处理副作用（状态变更告警、历史趋势记录、日志）
- **与 HealthChecker 的关系**：HealthChecker 只管"返回状态"，Hooks 管"副作用"——关注点分离，不耦合

**取舍表**：

| 回调 | 取舍 | 理由 |
|---|---|---|
| `on_health_change` | **保留（必须）** | 业务需要知道何时降级/恢复 |
| `on_keepalive_fail` | **保留（必须）** | 业务需要告警 |
| `on_instances_changed` | **保留（必须）** | 业务需要感知拓扑变化 |
| `on_register` | **保留（可选）** | 增强可观测性 |
| `on_deregister` | **保留（可选）** | 增强可观测性 |
| `on_governance_crash` | **保留（可选）** | 增强可观测性 |
| ~~`on_keepalive_success`~~ | **舍掉** | 太频繁（每 3s 一次），回调开销大 |
| ~~`on_pick` / `on_pick_fail`~~ | **舍掉** | 请求内高频操作，回调会增加开销 |
| ~~`on_instance_added` / `on_instance_removed`~~ | **舍掉** | 太细粒度，`on_instances_changed`（批量增删改）已够用 |
| ~~`on_pull_success`~~ | **舍掉** | 太频繁（每 2s 一次），同 `on_keepalive_success` |
| ~~`on_watch_connect` / `on_watch_disconnect`~~ | **舍掉** | 治理 worker 内部状态，业务不需要感知 |

### 8.6 宣告地址解析器（AdvertiseResolver）

```
interface AdvertiseResolver {
  resolve(): array;   -- 返回 {host: string, port: int}
}
```

- **内置默认**：`IniAdvertiseResolver`——读 INI `beacon.advertise_host`/`beacon.advertise_port`，未配置则自动探测（读网络接口，排除 loopback）
- **注入**：业务实现此接口或传闭包，自己决定怎么获取地址（云 metadata 服务/多网卡选择/任意逻辑）

**链式解析优先级**（注入优先，配置兜底）：

```
1. 注入了 AdvertiseResolver（OPT_ADVERTISE_RESOLVER）→ 用注入的
2. 配了 beacon.advertise_host_env → 从环境变量读（K8s downward API，如 POD_IP）
3. 配了 beacon.advertise_host → 用 INI 配置的
4. 都没配 → 自动探测（兜底，可能读到内网 IP，生产不推荐）
```

### 8.7 注入机制

**注入 API（PHP userland）**：

对象式注入（推荐）：

```php
// bootstrap.php（治理 worker fork 后执行一次）
Beacon::configure([
  Beacon::OPT_REGISTRY_ENDPOINT  => 'http://etcd:2379',
  Beacon::OPT_REGISTRY           => new MyConsulRegistry('http://consul:8500'),
  Beacon::OPT_HEALTH_CHECKERS    => [new MyBusinessHealthChecker()],
  Beacon::OPT_PERSISTENCE        => new MyRedisPersistence($redis),
  Beacon::OPT_KEEPALIVE_INTERVAL => 3,
  Beacon::OPT_PULL_INTERVAL      => 2,
]);
```

函数式注入（轻量场景，用匿名类包装 callable 为接口实现）：

```php
Beacon::setRegistry(new class implements RegistryAdapter {
    public function register(string $service, array $inst): bool { /* ... */ return true; }
    public function deregister(string $service, string $id): bool { /* ... */ return true; }
    public function keepalive(string $id, array $health): bool { /* ... */ return true; }
    public function discover(string $service): array { /* ... */ return []; }
    public function watch(string $service, callable $on_change): void { /* ... */ }
});
```

**注入时机**：

注入在 **治理 worker fork 后**完成（不是 MINIT——master 无 VM，不能执行 userland）。流程：

1. MINIT（master）：解析 INI → 初始化 shm → spawn 治理 worker（独立进程）
2. 治理 worker fork 后：初始化自己的 VM → 执行 `governance.php`（注入适配器对象）→ 进 timer 循环
3. FPM master fork FPM workers（注入对象不传递——FPM worker 不需要注入对象，只读 shm 取节点）

**关键约束**（PHP-FPM 进程模型决定）：
- master 无 VM，MINIT 不能执行 userland PHP——bootstrap 必须在治理 worker fork 后执行
- `auto_prepend_file` 是 per-request 的，每个请求都执行，**不适合**一次性注入
- 注入的 PHP 对象是 per-process 的，不能跨进程传递——但只有治理 worker 需要注入对象（注册/保活/发现），FPM worker 只读 shm，不碰注册中心

运行时不再改注入（避免热切换竞态）。

### 8.8 回调设计：必须、超时、异步化

**三个核心原则**：

| 原则 | 说明 | 落地方式 |
|---|---|---|
| **回调是必须的** | 扩展不内置任何注册中心协议，没有回调 = 无法工作 | bootstrap 阶段校验；缺失必须回调 → 扩展拒绝启动 |
| **回调有硬超时** | 防止用户代码阻塞拖垮治理 worker | PHP 层自治超时（ReactPHP 协程，HTTP 客户端自带 timeout） |
| **回调建议异步化** | 注册/保活/注销是"写"操作，不需要等待结果 | PHP 层用 ReactPHP 协程；C 层 fire-and-forget |

**异步化的具体含义**：

**不是 C 层多线程**（PHP 非线程安全），而是**调用语义上的 fire-and-forget**：

- C 扩展调 `on_register`/`on_keepalive`/`on_deregister` 时，**不等待副作用完成**
- 回调返回 `true`/`false` 或 `void` 都不影响治理 worker 的 timer 循环
- 实际的网络 I/O（HTTP 请求注册中心）由 PHP 层自行决定同步或异步

**为什么可以异步化？**

```
register/keepalive/deregister 的语义是"通知注册中心"，不是"事务操作"。
注册中心本身有 TTL/lease 机制兜底——即使一次 keepalive 丢了，下次补上就行。
真正需要同步的是 discover（必须拿到节点列表写 shm）。
```

**PHP 层异步化最佳实践**：

```php
<?php
// governance.php —— ReactPHP 协程版

use React\EventLoop\Loop;
use React\Http\Browser;

$loop = Loop::get();
$browser = new Browser($loop);

// keepalive timer（3s）
$loop->addPeriodicTimer(3.0, function () use ($browser, $endpoint) {
    $health = calc_health_from_shm();

    $browser->post("{$endpoint}/v3/lease/keepalive", [], json_encode($health))
        ->then(
            function ($res) { /* 成功 */ },
            function ($e) { error_log("keepalive failed: " . $e->getMessage()); }
        );
});

// discover timer（2s）
$loop->addPeriodicTimer(2.0, function () use ($browser, $endpoint) {
    $browser->post("{$endpoint}/v3/kv/range", [], json_encode($rangeBody))
        ->then(
            function ($res) {
                $nodes = parse_etcd_response($res->getBody());
                Beacon\Governance::storeNodes('calc', $nodes);
                Beacon\Governance::commit();
            },
            function ($e) { error_log("discover failed: " . $e->getMessage()); }
        );
});

$loop->run();
```

**核心机制**：
- `Browser->post()` 返回 Promise，HTTP IO 在 ReactPHP 的 event loop 中异步调度
- 一个请求超时只是那个 Promise reject，不影响其他 timer
- 治理 worker 永远不会因为"一个网络请求卡死"而停摆

---

## 九、API 与配置参考

### 9.1 PHP userland API

**核心 API**：

| API | 签名 | 用途 | 调用时机 |
|---|---|---|---|
| `Beacon::pick(string $service, array $opts = [])` | `?array` | 内置 LB 选一个实例。返回 `{id, host, port, status, methods, ...}` 或 null（无可用实例） | 请求内按需 |
| `Beacon::getInstances(string $service)` | `array` | 取该服务全部健康实例。返回 `[{id, host, port, status, methods, ...}, ...]` | 请求内按需 |
| `Beacon::ready()` | `bool` | 标记本实例预热完成，健康从 NOT_READY 转 OK。实现：写 shm `pool.ready = 1`，治理 worker 下次 keepalive 时读到并转 OK | 预热完成后调用一次 |
| `Beacon::reportHealth(array $health)` | `bool` | 业务主动报健康。实现：写 shm `pool.business_health = $health`，治理 worker 的 HealthChecker 链式执行时合并业务报的健康（与 FPM 池感知取最差） | 请求内按需 |
| `Beacon::deregister()` | `bool` | 手动注销。实现：写 shm `pool.deregister = 1`，治理 worker 下次 tick 时读到并执行 `registry.deregister()`。pool stop 时治理 worker 自动 deregister，无需手动调 | 极少 |
| `Beacon::status()` | `array` | 返回当前 pool 状态（governance_pid、governance_status、cache_age_seconds、mode 等），供 health check endpoint 调用 | 请求内按需 |

**治理 worker API（CLI SAPI 专用）**：

| API | 签名 | 用途 | 调用时机 |
|---|---|---|---|
| `Beacon\Governance::storeNodes(string $service, array $nodes)` | `bool` | 把 PHP 数组转为 C 结构体写入 shm（非激活 buffer） | 治理 worker discover 后 |
| `Beacon\Governance::commit()` | `bool` | 原子切 active buffer，FPM worker 立即可见新数据 | storeNodes 后 |
| `Beacon\Governance::calcHealth()` | `array` | 读 shm 自计数，算 pool 级健康状态 | 治理 worker keepalive 前 |

**pick() 的 opts**：

- `Beacon::OPT_EXCLUDE`: 已试节点 id 数组（failover 用）
- `Beacon::OPT_LB_STRATEGY`: 临时覆盖 LB 策略（`Beacon::LB_ROUND_ROBIN` / `Beacon::LB_RANDOM` / `Beacon::LB_WEIGHTED`）
- `Beacon::OPT_PREFER_HEALTHY`: bool，是否只选 HEALTHY（默认 true，DEGRADED 排后）

### 9.2 INI 指令（php.ini / FPM pool conf）

| 指令 | 默认 | 说明 |
|---|---|---|
| `beacon.enabled` | 0 | 开关 |
| `beacon.service_name` | - | 本服务名（provider 身份） |
| `beacon.advertise_host` | 自动探测 | 对外地址（peer 连此，非 FPM listen）。解析优先级：注入 `AdvertiseResolver` > `advertise_host_env` > 本 INI > 自动探测 |
| `beacon.advertise_host_env` | "" | 从环境变量读 advertise_host（K8s downward API，如 `POD_IP`）。优先级高于本 INI 的 `advertise_host` |
| `beacon.advertise_port` | 自动探测 | 对外端口 |
| `beacon.registry_endpoint` | "" | 注册中心地址。空 = 文件模式（默认） |
| `beacon.governance_bin` | "" | PHP CLI 路径，空 = 编译宏 `BEACON_PHP_BIN` |
| `beacon.governance_script` | "" | 治理脚本路径，空 = 内置默认（`/usr/share/php/beacon/governance.php`） |
| `beacon.keepalive_interval` | 3 | 保活间隔(s) |
| `beacon.pull_interval` | 2 | 拉取间隔(s) |
| `beacon.heartbeat_ttl` | 15 | 注册中心 lease/TTL(s) |
| `beacon.health_dead_threshold` | 3 | 连续 dead 次数才报 dead |
| `beacon.lb_strategy` | "round_robin" | 默认 LB 策略（`Beacon::LB_ROUND_ROBIN`/`RANDOM`/`WEIGHTED`） |
| `beacon.shm_key` | 自动 | IPC shm key |
| `beacon.log_file` | "" | 日志文件路径。空 = 写 stderr（systemd journal 捕获） |
| `beacon.log_level` | "warn" | 日志级别（debug/info/warn/error） |

**零配置模式**：

```ini
; 最小可运行配置
beacon.enabled = 1
beacon.service_name = "calc"
```

- `advertise_host` 空 → 自动探测（排除 loopback）
- `advertise_port` 空 → 自动探测（读 FPM listen port 或 80）
- `registry_endpoint` 空 → 文件模式（`/var/run/beacon/`）
- `governance_script` 空 → 内置脚本

### 9.3 预定义常量

对标 PHP 扩展惯例（`YAR_OPT_*` / `CURLOPT_*` / `Redis::OPT_*`），选项 key 用整数常量，枚举值用常量消除魔术字符串。

**选项 key 常量（`Beacon::OPT_*`，整数，用于 `setOpt()` 的 key）**：

| 常量 | 值 | 类型 | 说明 |
|---|---|---|---|
| `Beacon::OPT_ON_REGISTER` | 1 | callable\|null | 服务注册回调（软必须），`function(array $ctx): void` |
| `Beacon::OPT_ON_KEEPALIVE` | 2 | callable\|null | 保活回调（**硬必须**），`function(array $ctx): void` |
| `Beacon::OPT_ON_DISCOVER` | 3 | callable\|null | 节点发现回调（**硬必须**），`function(string $service): array` |
| `Beacon::OPT_ON_DEREGISTER` | 4 | callable\|null | 服务注销回调（软必须），`function(array $ctx): void` |
| `Beacon::OPT_ON_WATCH` | 5 | callable\|null | 节点监听回调（可选），`function(string $service, callable $onChange): void` |
| `Beacon::OPT_LB_STRATEGY` | 6 | int | LB 策略（`Beacon::LB_*` 枚举） |
| `Beacon::OPT_KEEPALIVE_INTERVAL` | 7 | int | 保活间隔(s) |
| `Beacon::OPT_PULL_INTERVAL` | 8 | int | 拉取间隔(s) |
| `Beacon::OPT_HEARTBEAT_TTL` | 9 | int | 注册中心 lease/TTL(s) |
| `Beacon::OPT_HEALTH_DEAD_THRESHOLD` | 10 | int | 连续 dead 次数才报 dead |
| `Beacon::OPT_EXCLUDE` | 11 | array | `pick()` 用：已试节点 id 数组（failover） |
| `Beacon::OPT_PREFER_HEALTHY` | 12 | bool | `pick()` 用：是否只选 HEALTHY（默认 true） |

**LB 策略枚举（`Beacon::LB_*`，用于 `OPT_LB_STRATEGY` 的值）**：

| 常量 | 值 | 说明 |
|---|---|---|
| `Beacon::LB_ROUND_ROBIN` | 1 | 轮询（默认） |
| `Beacon::LB_RANDOM` | 2 | 随机 |
| `Beacon::LB_WEIGHTED` | 3 | 加权（按节点健康/负载分配） |

**健康状态枚举（`Beacon::HEALTH_*`，用于 `HealthChecker::check()` 返回的 status）**：

| 常量 | 值 | 说明 |
|---|---|---|
| `Beacon::HEALTH_NOT_READY` | "not_ready" | 启动中/预热未完成，不接流量 |
| `Beacon::HEALTH_OK` | "ok" | 健康，接全量流量 |
| `Beacon::HEALTH_DEGRADED` | "degraded" | 饱和/降级，接少量流量（降优先级） |
| `Beacon::HEALTH_DEAD` | "dead" | 死亡，摘除不接流量 |

常量在扩展 MINIT 阶段用 `REGISTER_LONG_CONSTANT` / `REGISTER_STRING_CONSTANT` 注册，PHP userland 通过 `Beacon::OPT_*` / `Beacon::LB_*` 等访问。对标 `YAR_OPT_PACKAGER` 的注册方式。

### 9.4 错误处理

**遵循 PHP 扩展惯例：用返回值表示状态，不用异常表示状态。**

```php
// Redis —— 返回 false 表示失败
$val = $redis->get('key');  // false = key 不存在或连接失败

// curl —— 返回 false
$res = curl_exec($ch);  // false = 请求失败

// beacon —— 返回 null 表示无节点
$node = Beacon::pick('user');  // null = 无可用节点
```

- `pick()` 返回 `?array`，null = 无可用节点
- `getInstances()` 返回 `array`，空数组 = 无节点
- 扩展内部错误（如 shm 损坏）用 `php_error_docref` 写日志，返回 false/null
- 一期不注册自定义异常类

**业务代码必须处理 null**：

```php
$node = Beacon::pick('user');
if ($node === null) {
    // 方案 A：静态兜底（适合有固定 backup 节点的场景）
    $node = ['host' => '127.0.0.1', 'port' => 8080];

    // 方案 B：返回错误（推荐）
    // throw new ServiceUnavailableException('user service: no available nodes');
}
```

---

## 十、PHP 使用指南

### 10.1 安装

**源码编译（推荐）**：

```bash
git clone https://github.com/beacon/php-extension.git
cd php-extension
phpize
./configure --with-php-config=/usr/bin/php-config8.2
make
sudo make install
```

`make install` 会安装：
- `beacon.so` → PHP 扩展目录
- `governance.php` → `/usr/share/php/beacon/governance.php`（内置治理脚本）
- `FileRegistry.php` → `/usr/share/php/beacon/adapters/FileRegistry.php`（内置文件注册中心）

**GitHub Release 预编译（二期）**：

```bash
curl -fsSL https://beacon.sh/install.sh | bash
```

**PECL（三期，不主推）**：

```bash
pecl install beacon
```

### 10.2 配置

**最小配置（零配置模式）**：

`php.ini` 或 FPM pool conf：

```ini
extension=beacon.so
beacon.enabled = 1
beacon.service_name = "calc"
```

**零配置模式行为**：
- `advertise_host` 空 → 自动探测（排除 loopback）
- `advertise_port` 空 → 自动探测（读 FPM listen port 或 80）
- `registry_endpoint` 空 → 文件模式（`/var/run/beacon/`）
- `governance_script` 空 → 内置脚本

**完整配置**：

```ini
extension=beacon.so
beacon.enabled = 1
beacon.service_name = "calc"              ; 本服务名（作为 provider 时）
beacon.advertise_host = "10.0.0.5"        ; 对外地址（peer 连这个，不是 FPM listen 地址）
beacon.advertise_port = 8888             ; 对外端口（通常是前置 nginx 端口）
beacon.registry_endpoint = "http://etcd:2379"  ; 注册中心地址（空 = 文件模式）
beacon.governance_script = "/etc/beacon/my-governance.php"  ; 自定义治理脚本（可选）
beacon.keepalive_interval = 3
beacon.pull_interval = 2
beacon.heartbeat_ttl = 15
beacon.log_file = "/var/log/beacon/governance.log"  ; 日志文件（空 = stderr）
beacon.log_level = "warn"
```

### 10.3 三层使用模型

| 层 | 谁做 | 何时 | PHP 代码 |
|---|---|---|---|
| **自注册** | 扩展自动 | FPM pool 启动 | 无（零代码，扩展 fork worker 自动注册） |
| **保活/发现** | 治理 worker 自动 | 后台循环 | 无（worker 跑 timer，PHP 不感知） |
| **取节点** | PHP 业务代码按需 | 请求内需要调 peer 时 | `Beacon::pick($service)` 一行 |

**关键**：PHP 业务代码只在**取节点**这一步显式调 API。自注册和保活是扩展后台自动做的，PHP 代码完全不碰。

### 10.4 不注入任何实现（文件模式，零依赖）

**回调是必须的**——扩展不内置任何注册中心协议，不写 `bootstrap.php` 注入回调，扩展无法工作（治理 worker 启动时校验回调，缺失硬必须回调则报错退出）。

但**文件模式**（默认）下，内置 `governance.php` 已包含 `FileRegistry`，零配置即可工作：

```php
<?php
// 业务代码只需：
$node = Beacon::pick('user');   // 从 shm 读，内置 LB 选一个健康节点
$client = new Yar_Client("http://{$node['host']}:{$node['port']}/user");
$result = $client->getProfile($uid);
```

**调试**：

```bash
# 查看本机注册了哪些服务
ls -la /var/run/beacon/

# 查看某个服务的健康状态
cat /var/run/beacon/calc.json

# 查看治理 worker 是否存活
ps aux | grep beacon-governance
```

### 10.5 注入实现（需要定制时）

**自定义注册中心（如 Consul）**：

`/etc/beacon/bootstrap.php`：

```php
<?php
Beacon::configure([
    Beacon::OPT_REGISTRY       => new MyConsulRegistry('http://consul:8500'),
    Beacon::OPT_HEALTH_CHECKERS => [new MyBusinessHealthChecker()],
]);
```

或逐项设置：

```php
<?php
Beacon::setOpt(Beacon::OPT_REGISTRY, new MyConsulRegistry('http://consul:8500'));
```

**自定义治理脚本**：

```ini
beacon.governance_script = "/etc/beacon/etcd-governance.php"
```

```php
<?php
// /etc/beacon/etcd-governance.php
require '/vendor/autoload.php';
use Beacon\Adapter\EtcdRegistry;

Beacon::configure([
    Beacon::OPT_REGISTRY => new EtcdRegistry('http://etcd:2379'),
]);
```

### 10.6 与 Yar RPC 协作

**作为 Yar 服务端（provider）——零代码**：

PHP 服务用 `Yar_Server::handle($service_obj)` 暴露 RPC，**代码不变**。扩展在 FPM pool 启动时自动把"本服务 calc 在 10.0.0.5:8888"注册到注册中心。

```php
// calc.php — Yar 服务端，原样不变
$calc = new Calculator();
$server = new Yar_Server($calc);
$server->handle();
```

扩展读 INI 的 `service_name` + `advertise_host:port`，注册到注册中心。peer（其他 PHP 或 beacon）从注册中心发现 calc 服务在此地址。

**作为 Yar 客户端（consumer）——client 侧 LB + failover**：

```php
// 请求内调 user 服务
$tried = [];
while ($node = Beacon::pick('user', [Beacon::OPT_EXCLUDE => $tried])) {
    $tried[] = $node['id'];
    $client = new Yar_Client("http://{$node['host']}:{$node['port']}/user");
    try {
        $result = $client->getProfile($uid);
        break;  // 成功
    } catch (Yar_Server_Exception $e) {
        continue;  // 失败，pick 排除已试，重试下一个
    }
}
if (!isset($result)) {
    throw new ServiceUnavailableException("user 服务无可用节点");
}
```

这是 **client 侧负载均衡 + 故障转移**——PHP 进程内完成，不经网关。对标 Go gRPC client 的 round-robin + retry。

**协议无关性**：

`Beacon::pick()` 返回的是节点（host/port/health），不创建 Yar client。PHP 代码自己创建 client——所以同一套发现机制可用于 gRPC-PHP、HTTP API、Thrift，不绑定 Yar：

```php
// gRPC-PHP 消费者同样用 Beacon::pick
$node = Beacon::pick('order');
$client = new Grpc\BaseStub("{$node['host']}:{$node['port']}");
```

### 10.7 服务声明机制

**三层服务声明**：

```
层 1（INI 静态）    beacon.service_name = "calc"           → 治理 worker 启动即注册
层 2（bootstrap）   Beacon::serveAll(['calc'])   → 治理 worker 启动时注册（PHP 写）
层 3（入口文件）    Beacon::serve('calc', $obj)             → 请求来时声明，治理 worker 下次 tick 注册
```

- **层 1/2 解决冷启动**：pool 刚启动还没请求来，治理 worker 就能注册（避免"没人来就不注册，不注册就没人来"的死锁）
- **层 3 解决方法列表自动反射 + handler 附近声明**（符合"声明与 handler 同源"的期望）

**`Beacon::serve()` API**：

```php
// 入口文件（如 /var/www/html/calc.php）
$svc = new Calculator();
Beacon::serve('calc', $svc);              // 声明服务 + 反射方法列表
(new Yar_Server($svc))->handle();
```

**扩展行为**（C 函数实现）：
- `Beacon::serve($name, $obj?)`：写 shm `services[$name] = {methods: [...]}`，幂等（先读 shm 看有没有，没有才写）
- 有 `$obj` 时：C 层遍历 `zend_class_entry->function_table`，提取 public 方法名，写 shm `services[$name].methods`
- 无 `$obj` 时：只声明服务名，方法列表为空

**C 层 per-script 缓存**：同 worker 连续处理同一脚本时，第 2-N 次直接返回，不重复写 shm。

**推荐用法**：

```php
// /etc/beacon/bootstrap.php（治理 worker 启动时执行一次）
Beacon::configure([
    Beacon::OPT_REGISTRY        => new MyEtcdRegistry('http://etcd:2379'),
    Beacon::OPT_HEALTH_CHECKERS => [new MyBusinessHealthChecker()],
]);
Beacon::serveAll(['calc', 'user']);  // 静态声明，启动即注册（冷启动不断流）

// /var/www/html/calc.php（入口文件，per-request 但幂等）
$svc = new Calculator();
Beacon::serve('calc', $svc);              // 动态声明 + 方法反射，治理 worker 补充注册
(new Yar_Server($svc))->handle();
```

### 10.8 健康检查

**内置 FPM 池感知**：

扩展内置 `FpmPoolHealthChecker`——master 视角自感知（RINIT/RSHUTDOWN 自计数 busy/idle + shm 汇总），报 FPM 池级健康。

**业务级健康检查**：

```php
class MyBusinessHealthChecker implements HealthChecker {
    public function check(): array {
        // 检查 DB 连接
        if (!$this->db->ping()) {
            return ['status' => Beacon::HEALTH_DEAD, 'metrics' => ['db' => 'down']];
        }
        // 检查缓存
        if (!$this->cache->ping()) {
            return ['status' => Beacon::HEALTH_DEGRADED, 'metrics' => ['cache' => 'cold']];
        }
        return ['status' => Beacon::HEALTH_OK, 'metrics' => []];
    }
}

Beacon::registerHealthChecker(new MyBusinessHealthChecker());
```

**预热完成标记**：

```php
// 业务预热完成后调用
Beacon::ready();  // 健康从 NOT_READY 转 OK
```

### 10.9 故障排查

**查看治理 worker 状态**：

```bash
# 治理 worker 是否存活
ps aux | grep beacon-governance

# 查看治理 worker 日志
tail -f /var/log/beacon/governance.log | jq .

# 查看 FPM 扩展错误
tail -f /var/log/php-fpm/www-error.log | grep beacon
```

**查看注册状态**：

```bash
# 文件模式
cat /var/run/beacon/calc.json

# etcd 模式
etcdctl get /beacon/inst/calc/ --prefix
```

**查看 pool 健康状态**：

```php
<?php
// /health.php
$status = Beacon::status();
header('Content-Type: application/json');
echo json_encode($status);

// 输出：
// {
//   "pool": "www",
//   "pool_busy": 8,
//   "pool_idle": 2,
//   "governance_pid": 12345,
//   "governance_status": "running",
//   "cache_age_seconds": 5,
//   "mode": "full"
// }
```

---

## 十一、集成与协同

### 11.1 与 Yar RPC 协作

**作为 Yar 服务端（provider）——零代码**：

PHP 服务用 `Yar_Server::handle($service_obj)` 暴露 RPC，**代码不变**。扩展在 FPM pool 启动时自动把"本服务 calc 在 10.0.0.5:8888"注册到注册中心。

```php
// calc.php — Yar 服务端，原样不变
$calc = new Calculator();
$server = new Yar_Server($calc);
$server->handle();
```

扩展读 INI 的 `service_name` + `advertise_host:port`，注册到注册中心。peer（其他 PHP 或 beacon）从注册中心发现 calc 服务在此地址。

**advertise 地址的必要性**：FPM 通常 listen `127.0.0.1:9000`，peer 连不到。前置 nginx 在 `10.0.0.5:8888` 转发到 FPM。注册的是 advertise 地址（nginx），不是 listen 地址（FPM）。对标 Consul 的 `advertise_addr`。

**注册内容**（写入注册中心的完整结构）：

```
/beacon/inst/{service}/{instance_id} = {
  host: "10.0.0.5",           ← advertise_host（前置 nginx 地址，非 FPM listen）
  port: 8888,                  ← advertise_port（前置 nginx 端口）
  status: "ok",                ← FPM pool 健康（自计数汇总，非单个 worker）
  methods: ["add", "sub"],     ← 服务方法列表（Beacon::serve 反射）
  registered_at: 1692800000,   ← 注册时间戳
  lease_id: 12345              ← etcd lease ID（保活用）
}
```

**实例 ID**：`{service_name}-{advertise_host}-{advertise_port}`，如 `calc-10.0.0.5-8888`。同一 host:port 只有一个 FPM pool 提供同一个服务，ID 唯一。对标 Consul 的 `{service}-{node}-{port}`。

**注册的是 FPM pool 的健康，不是单个 worker 的健康**——`FpmPoolHealthChecker` 读 shm 自计数（busy/idle/throughput），报整个 pool 的健康。外部连的是 advertise 地址（nginx），不是单个 worker。

**作为 Yar 客户端（consumer）——client 侧 LB + failover**：

```php
// 请求内调 user 服务
$tried = [];
while ($node = Beacon::pick('user', [Beacon::OPT_EXCLUDE => $tried])) {
    $tried[] = $node['id'];
    $client = new Yar_Client("http://{$node['host']}:{$node['port']}/user");
    try {
        $result = $client->getProfile($uid);
        break;  // 成功
    } catch (Yar_Server_Exception $e) {
        continue;  // 失败，pick 排除已试，重试下一个
    }
}
if (!isset($result)) {
    throw new ServiceUnavailableException("user 服务无可用节点");
}
```

这是 **client 侧负载均衡 + 故障转移**——PHP 进程内完成，不经网关。对标 Go gRPC client 的 round-robin + retry。

**协议无关性**：

`Beacon::pick()` 返回的是节点（host/port/health），不创建 Yar client。PHP 代码自己创建 client——所以同一套发现机制可用于 gRPC-PHP、HTTP API、Thrift，不绑定 Yar：

```php
// gRPC-PHP 消费者同样用 Beacon::pick
$node = Beacon::pick('order');
$client = new Grpc\BaseStub("{$node['host']}:{$node['port']}");
```

**可选高层封装（独立 package，非扩展）**：

扩展只提供 `pick()`（协议无关）。若要"一行调 Yar + 自动重试"，那是独立 package `beacon-yar` 的事，不塞进扩展（保持扩展协议无关）：

```php
// beacon-yar package（非扩展本体）
$result = BeaconYar::call('user', 'getProfile', [$uid]);  // pick + client + retry 封装
```

### 11.2 与 beacon 网关的协同

**共享 etcd，职责分离**：

扩展和 beacon **用同一个 etcd**，但职责不重叠：

| 角色 | 职责 | 对 etcd |
|---|---|---|
| php-beacon-extension | PHP 自注册 + 保活（带自报健康）+ 本地节点缓存 | **写**（register/keepalive）+ 读（discover） |
| beacon 网关 | 路由/LB/故障转移/协议转化 | **读**（读 PHP 自注册的状态做路由决策） |

**自注册 > 主动探活**：

扩展让 PHP 自报健康（master 视角 + 业务级），keepalive 携带权威健康数据。这**彻底消灭 beacon 主动探活的盲区**：
- 无 in-band 排队盲区（PHP 自己报，不用 beacon 探）
- 无网络分区误判（PHP 自己知道活着，不靠 beacon 猜）
- 无饱和检测延迟（PHP 实时报池状态，不等 3s 探活轮）

beacon 从"猜 PHP 健不健康"升级为"读 PHP 自报的健康"。

**两种部署形态**：

**形态 A：扩展 + beacon 共存**（推荐）
- PHP 自注册到 etcd，beacon 从 etcd 读做路由
- 扩展消灭探活盲区，beacon 做网关层路由/LB/协议转化
- 两者协同，各取所长

**形态 B：仅扩展，无 beacon**
- PHP 自注册 + 自发现 + 本地缓存，PHP 直接调 peer（Yar client → peer Yar server）
- 纯 Go 模型，beacon 退化为只做协议转化（gRPC↔Yar）+ ingress
- 适合 PHP 间直连、不需要网关层路由的场景

**数据流闭环**：

```
PHP(FPM+扩展) ──register/keepalive(带自报健康)──→ etcd
                                                      ↑
                          beacon 网关 ──read(路由决策)──┘
                              ↓
                     balancer 选健康 PHP 实例
                              ↓
                     请求 → PHP-FPM worker
```

PHP 自报健康（权威）→ etcd ← beacon 读（路由）。**自注册消灭探活盲区**：PHP 自己知道活着，不靠 beacon 猜。beacon 从"探活 PHP"升级为"读 PHP 自报"。

---

## 十二、验收标准与取舍边界

### 12.1 验收标准

**功能验收**：

1. FPM 启动，扩展 spawn 治理 worker（独立进程），worker 自注册到注册中心，beacon 能读到该实例
2. 注入自定义 `HealthChecker`（返回 degraded），keepalive 携带 degraded，beacon 读到并降权路由
3. 注入自定义 `Persistence`（存 Redis），FPM worker `getInstances` 从 shm 读到节点（shm 是 L1，Redis 是 L2）
4. 不注入任何实现，用内置默认（文件模式 + FPM 池感知 + shm），开箱即用
5. 杀治理 worker，master 重启它，重新注册，实例不被误摘（TTL 兜底）
6. 注入的 `health_checker` 抛异常，扩展降级到内置默认健康，不崩 worker

**性能验收**：

| 指标 | 目标 | 测量方式 |
|---|---|---|
| 自计数开销 | 10k req/s 下 < 1ms/s | `ab -n 100000 -c 100` 压测，对比扩展启用前后的 QPS |
| `pick()` 延迟 | < 1μs | 基准测试，shm 读 + 进程内 LB |
| shm 占用 | < 10MB（100 服务 × 10 实例） | `ipcs -m` 查看 shm 段大小 |
| 冷启动 | pool 启动到首次注册 < 1s | 日志时间戳差 |

**稳定性验收**：

| 场景 | 目标 | 验证方式 |
|---|---|---|
| 治理 worker 崩溃 | 15s 内重启，实例不被误摘 | `kill -9` 治理 worker，观察 etcd 实例是否存活 |
| 注册中心不可达 | 用本地缓存继续服务，不注册但能发现已有节点 | 断 etcd 网络，观察 `pick()` 是否正常 |
| 健康检查器全部失败 | 降级为内置默认（FPM 池感知） | 注入的 HealthChecker 全部抛异常 |
| shm 不可用 | 直接查注册中心（慢但能用） | 模拟 shm 损坏 |
| 治理 worker 反复崩溃 | 停止重启，降级为"无治理模式"（FPM 正常服务请求） | 连续 kill 治理 worker 5 次 |

### 12.2 取舍与边界

**治理 worker 崩溃**：

worker 崩溃 = 无保活 = 实例在注册中心 TTL 过期被摘。缓解：
- master 从 shm 读 `governance_pid`，`kill(pid, 0)` 检测，崩溃自动重启（重启后重新 register，幂等）
- 重启窗口内 keepalive 间断，靠注册中心 TTL 兜底（etcd lease TTL 设宽松些，如 15s）
- 极端兜底：master 线程跑一个最简 C 级 keepalive（不依赖 worker，路线 A 兜底），仅续 lease 不调 userland

**注入的 PHP 实现崩溃**：

业务注入的 `health_checker.check()` 抛异常 = keepalive 失败。扩展用 `try/catch`（C 层 `zend_call_function` 包 `try`）隔离，异常时用内置默认健康（FPM 池感知）兜底，不让 userland 崩溃拖垮治理 worker。

**IPC 延迟**：

FPM worker `getInstances` 读 shm 是共享内存直读，无 IPC 调用，纳秒级。watch 推送到 shm 有毫秒级延迟（worker → shm 写 → FPM worker 读），靠 beacon 故障转移兜底（请求失败重试）。

**不做的事**：

- **不做协议转换**：那是 beacon/grpc-yar-bridge 的活
- **不做请求路由/LB**：那是 beacon 网关的活（形态 B 下 PHP 自己 LB，但那是 client 侧负载均衡，不是网关）
- **不做熔断/限流**：可观测性层，留给 beacon Phase 4 或独立组件
- **不绑定 Yar**：服务任意 PHP 服务，Yar 只是消费者之一

**完整降级链**：

```
1. 注入的 PHP 实现抛异常
   → C 层 zend_try 隔离 → 降级内置默认（FPM 池感知）

2. 治理 worker 崩溃
   → master kill(pid, 0) 检测 → 重启 → 重新 register（幂等）
   → 重启窗口内 etcd lease TTL(15s) 保活，不误摘

3. 治理 worker 反复崩溃（连续 5 次）
   → 停止重启 → 降级为"无治理模式"（FPM 正常服务请求，只是没有服务治理功能）
   → 记日志（error 级别），告警

4. shm 损坏
   → 双缓冲互救：激活 buffer 坏 → 读备份 buffer
   → 文件模式恢复：治理 worker 重启后从文件恢复 shm

5. etcd 不可达
   → keepalive 失败累积 → lease 过期被摘
   → 但 FPM worker 仍能用 shm 里的缓存（继续服务已有节点）
   → beacon 侧 L2 TTL / 故障转移兜底

6. FPM master 崩溃
   → FPM 本身宕（扩展管不了）
   → beacon 探活/lease 过期检测，摘除该实例
```

**核心原则**：扩展的任何异常都不应导致 FPM 无法服务请求。最坏情况（治理 worker 反复崩溃 + shm 损坏 + etcd 不可达）下，FPM 仍能正常服务请求——只是没有服务治理功能（不注册、不保活、不发现新节点），但已有节点的缓存仍能用。

### 12.3 日志记录点

异常情况必须记日志，`beacon.log_level` 控制级别：

| 事件 | 日志级别 | 内容 |
|---|---|---|
| 治理 worker 启动 | info | `governance worker started, pid={pid}` |
| 治理 worker 崩溃 | error | `governance worker crashed, reason={reason}, restarting` |
| 注册成功 | info | `registered: service={service}, instance={instance_id}` |
| 注册失败 | error | `register failed: service={service}, error={error}` |
| keepalive 失败 | warn | `keepalive failed: service={service}, consecutive={n}` |
| 健康状态变更 | info | `health changed: {old} → {new}, metrics={...}` |
| shm 损坏 | error | `shm corrupted, rebuilding from registry` |
| 注入的 PHP 实现抛异常 | warn | `injected {spi} threw {exception}, falling back to builtin` |
| 实例增删 | debug | `instances changed: added={n}, removed={n}` |

### 12.4 防御性编程

扩展自身异常不影响 FPM：

| 位置 | 风险 | 防御 |
|---|---|---|
| MINIT | 段错误导致 master 崩溃 | 只做最小初始化（注册常量/INI/shm），不做复杂逻辑 |
| RINIT/RSHUTDOWN | 段错误导致 worker 崩溃 | 只做原子操作（shm inc/dec），`zend_try` 包裹，异常不影响请求处理 |
| 治理 worker | 崩溃导致无保活 | master 从 shm 读 `governance_pid`，`kill(pid, 0)` 检测 → 重启 → 重新 register（幂等） |
| 注入的 PHP 实现 | 抛异常拖垮治理 worker | C 层 `zend_call_function` 包 `zend_try` → 降级内置默认 |

---

## 十三、业界对标与设计溯源

### 13.1 健康检查：应用层自省 vs 传输层探测

**业界共识：连接可达 ≠ 服务可用。**

gRPC Health Checking Protocol（`grpc.health.v1`）的核心设计：健康检查本身是一个标准 gRPC 服务（`Check` + `Watch`），服务端**自主**决定 `SERVING`/`NOT_SERVING`——基于应用层状态（DB 连接池、缓存预热、配置加载），不是 TCP 探测。文档原文：*"A server may choose to reply 'unhealthy' because it is not ready to take requests, it is shutting down or some other reason."*

K8s 三层探针进一步细化：`startup`（启动完成？）→ `liveness`（还活着？）→ `readiness`（能接流量？）。`readiness` 失败**不杀容器**，只移出 Endpoints（停止流量）——优雅处理临时不可用，不重启。

Envoy 的 `degraded` 状态：返回 `x-envoy-degraded` 头的主机**仍参与 LB 但降优先级**，仅在健康主机不足时接流量。饱和不是摘除，是降级——这是业界共识。

**beacon 对标**：自计数健康 = 应用层自省（RINIT/RSHUTDOWN 数 busy/idle，治理 worker 读 shm 报告），三态（ok/degraded/dead）= gRPC SERVING/—/NOT_SERVING + K8s passing/warning/critical + Envoy healthy/degraded/unhealthy。degraded 降优先级 = Envoy 共识。

### 13.2 服务发现：抽象接口 + 可插拔后端

**go-kit `sd` 包**的抽象层次（27.4k stars，Go 微服务标杆）：

```
Instancer（实例监听）→ EndpointCache（缓存管理）→ Endpointer（对外提供）→ Balancer（选址）→ Retry（重试）
Registrar（注册/注销）
Factory（instance → endpoint，连接建立与发现解耦）
```

每个注册中心后端（consul/etcdv3/zk/eureka/dnssrv）实现 `Instancer` + `Registrar`，可自由替换。

**grpc-go** 的抽象：`resolver.Builder`/`Resolver`（服务发现）+ `balancer.Balancer`/`Picker`（负载均衡），客户端侧 LB 的标杆。

**beacon 对标**：

| beacon | go-kit | grpc-go | 语义 |
|---|---|---|---|
| `RegistryAdapter` | `Registrar` + `Instancer` | `resolver.Resolver` | 注册 + 发现 |
| `Persistence` | `EndpointCache` | — | 本地缓存 |
| `Beacon::pick()` | `Balancer.Balance()` | `Picker.Pick()` | 选址 |
| `Beacon::pick(exclude)` | `lb.Retry` | retry policy | failover |

**设计差异**：go-kit 的 `Factory` 把 instance 转为 endpoint（管连接），beacon 不管连接（PHP 的 `Yar_Client` 自管 keepalive pool），只管选址。这是合理的——PHP 生态的连接管理在 Yar client 侧，beacon 不越权。

### 13.3 进程模型：master 无 VM → fork 有 VM 子进程

**Swoole** 的 Master-Worker 模型：Master 进程纯 C（epoll/kqueue 事件循环，无 PHP VM），fork 出 Worker（有独立 VM）+ TaskWorker（后台任务进程）。IPC 三种：管道（socketpair）、共享内存（`Swoole\Table`/`Swoole\Atomic`）、消息队列（sysvmsg）。Master 崩溃不影响已建立的连接，Worker 崩溃 Master 自动重启。

**PHP-FPM** 的进程模型：master 管理（`fpm_children_create` fork workers），worker 有 VM 处理请求。master 无 VM（不执行 userland）。扩展只有 MINIT/RINIT/RSHUTDOWN/MSHUTDOWN 钩子，MINIT 在 master 执行（fork workers 前），RINIT/RSHUTDOWN 在 worker 执行（per-request）。

**beacon 对标**：治理 worker = Swoole TaskWorker（独立进程，有 VM，做后台工作）。sysv shm = Swoole Table/Atomic（跨进程共享，无锁）。MINIT spawn 治理 worker = Swoole master fork TaskWorker。关键约束一致：master 无 VM → 后台工作必须在独立进程里做。

### 13.4 注册中心：lease/TTL = 健康代理

**etcd lease**：`LeaseGrant(TTL=15s)` + `Put(key, value, lease)`。keepalive 续期 → lease 存活 → key 存在。不续期 → lease 过期 → etcd 自动删 key。lease TTL = 健康代理，无需主动 delete。

**Consul**：`DeregisterCriticalServiceAfter` 字段——服务 critical 状态超过该时间自动注销。agent 定期执行检查（HTTP/TCP/Script），聚合状态 passing/warning/critical。

**K8s**：`readinessProbe` 失败 → Pod 移出 Endpoints → Service 不转发流量。probe 恢复 → Pod 重新加入 Endpoints。

**beacon 对标**：etcd lease TTL(15s) = 健康代理。keepalive 携带健康数据（自报，不是探活）。治理 worker 崩溃 → 不续期 → lease 过期 → etcd 自动删 health key → beacon 摘除该实例。三层兜底：主动 deregister（pool stop）> lease TTL 过期（治理 worker 崩溃）> beacon 侧探活（最后防线）。

### 13.5 启动就绪：startup 窗口

**K8s startup probe**：为慢启动应用提供专属启动检测窗口。`startupProbe` 成功一次后，检测权交给 `liveness`/`readiness`。启动期间不杀容器（`failureThreshold × periodSeconds` 的宽限窗口）。

**beacon 对标**：注册时初始状态 `not_ready`（不接流量），预热完成（`Beacon::ready()` 或首次 keepalive 成功）后转 `ok`。避免"注册了但还没准备好"的窗口——对标 K8s startup probe 的思路。

### 13.6 值得看的文档

| 文档 | 价值 | 链接 |
|---|---|---|
| gRPC Health Checking Protocol | 健康检查=标准 RPC 服务，应用层自省 vs 传输层探测的区分 | https://github.com/grpc/grpc/blob/master/doc/health-checking.md |
| K8s Liveness/Readiness/Startup Probes | 三层探针设计，readiness 失败移出 Endpoints 不杀容器 | https://kubernetes.io/docs/tasks/configure-pod-container/configure-liveness-readiness-startup-probes/ |
| go-kit `sd` 包 | 服务发现抽象接口标杆（Instancer/Registrar/Factory/Endpointer/Balancer） | https://github.com/go-kit/kit/tree/master/sd |
| Consul Service Registration API | 服务注册带检查，DeregisterCriticalServiceAfter 超时注销 | https://developer.hashicorp.com/consul/api-docs/agent/service |
| Envoy Outlier Detection | 主动+被动健康检查，degraded 降优先级不摘除，指数退避恢复 | https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/upstream/outlier |
| Swoole Process 文档 | master 无 VM → fork 有 VM 子进程的 PHP 标杆，IPC 三种机制 | https://wiki.swoole.com/#/process |
| grpc-go resolver | 客户端侧服务发现+LB 抽象（resolver.Builder/Resolver + balancer.Balancer/Picker） | https://github.com/grpc/grpc-go/tree/master/resolver |
| PHP 内部生命周期 | MINIT/RINIT/RSHUTDOWN/MSHUTDOWN 钩子时机与约束 | https://www.php.net/manual/en/internals2.structure.lifecycle.php |

### 13.7 可借鉴的优秀设计思路

1. **健康检查是应用层自省，不是传输层探测**（gRPC Health）——服务端自主决定 SERVING/NOT_SERVING，比 TCP 探测语义丰富。beacon 自计数正是此思路。

2. **degraded 降优先级，不摘除**（Envoy）——饱和的节点还在处理，只是少分配流量。摘除会导致雪崩（流量转移到其他节点）。beacon 三态路由 ok 优先 degraded 次之。

3. **readiness 失败不杀容器**（K8s）——临时不可用（加载配置、等外部服务）不应重启，只隔离流量。beacon 的 not_ready → ok 状态转换正是此思路。

4. **抽象接口 + 可插拔后端**（go-kit sd）——`Instancer`/`Registrar` 统一接口，各注册中心独立实现。beacon 的 `RegistryAdapter` SPI 正是此思路。

5. **连接建立与发现解耦**（go-kit Factory）——发现返回地址，连接由调用方管。beacon 的 `pick()` 返回节点信息，Yar_Client 自管连接池。

6. **lease/TTL = 健康代理**（etcd）——不续期自动删 key，无需主动 delete。三层兜底（主动 deregister > TTL 过期 > 探活）。

7. **指数退避恢复**（Envoy）——驱逐时间 = base × 连续驱逐次数，恢复后逐步减。避免反复抖动。beacon 网关侧可借鉴。

8. **master 无 VM → fork 有 VM 子进程**（Swoole）——后台工作不能在 master 做（无 VM），fork 子进程（有 VM）做。beacon 治理 worker 正是此思路。

### 13.8 最佳工程实践

1. **SPI + 依赖注入**：扩展定义语义接口（`RegistryAdapter`/`HealthChecker`/`Persistence`/`Hooks`/`AdvertiseResolver`），内置默认实现，允许业务层注入。不强绑定注册中心——对标 go-kit 的可插拔后端、Java SPI（Service Provider Interface）。

2. **请求路径零 I/O**：FPM worker 请求路径只碰 shm（原子操作 + 读），所有注册中心 I/O 在治理 worker。10k req/s 下自计数总开销 ≈ 0.4ms/s，可忽略。对标 Envoy 的数据面零阻塞。

3. **进程隔离**：治理 worker 和 FPM worker 是独立进程——worker 崩不影响治理（治理继续保活），治理崩不影响 worker（worker 仍服务请求，节点缓存陈旧直到重启）。对标 Swoole 的进程隔离。

4. **幂等注册**：治理 worker 崩溃重启后重新 register，同 service+id 覆盖不产生重复。对标 Consul `replace-existing-checks`。

5. **三层兜底**：主动 deregister（pool stop）> lease TTL 过期（治理 worker 崩溃）> beacon 侧探活（最后防线）。任何一层失效，下一层接住。

6. **声明与执行分离**：PHP 入口文件 `Beacon::serve()` 只声明（写 shm），治理 worker 执行注册（调 RegistryAdapter）。声明在 handler 附近，执行在后台——不干扰请求路径。

### 13.9 领域思想美学

**"服务知道自己活着"**——这是 beacon 的核心哲学。传统探活是"猜"（beacon 探 PHP，PHP 被动响应），自注册是"说"（PHP 主动报 beacon，beacon 被动听）。自报消灭探活盲区：in-band 排队、网络分区假阴性、饱和检测延迟——这些探活猜不到的，自报知道。

**"连接可达 ≠ 服务可用"**（gRPC Health）——TCP 连通不代表应用准备好处理请求。应用层自省（DB 连了吗？缓存热了吗？依赖服务通吗？）比传输层探测语义丰富。beacon 的 `HealthChecker` SPI 允许业务注入应用级健康判断。

**"饱和不是死亡"**（Envoy degraded）——busy 率高的 FPM 还在处理，只是该少分配流量。摘除会导致流量转移到其他节点，可能引发雪崩。降优先级是更优雅的处理——degraded 仍参与 LB，只是排在 ok 后面。

**"声明在源头，执行在后台"**——服务声明在 handler 入口文件（`Beacon::serve`），注册执行在治理 worker。声明与 handler 同源（改入口即改声明），执行脱离请求路径（不干扰 worker）。这是关注点分离的体现。

**"lease 是健康代理"**（etcd）——lease TTL = 健康的代理人。keepalive 续期 = 健康，不续期 = 死亡，etcd 自动清理。不需要主动 delete，不需要探活轮询——lease 机制把健康检查简化为"续期 or 不续期"的二选一。

### 13.10 服务治理领域概念词汇表

| 概念 | 英文 | 业界定义 | beacon 对应 |
|---|---|---|---|
| 服务注册 | Service Registration | 服务实例启动时向注册中心登记自己的地址和元数据 | `Beacon::serve()` / `Beacon::serveAll()` |
| 服务发现 | Service Discovery | 客户端从注册中心查询可用服务实例列表 | `Beacon::getInstances()` |
| 健康检查 | Health Check | 探测服务实例是否具备处理请求的能力 | `HealthChecker::check()` |
| 负载均衡 | Load Balancing | 从多个健康实例中选择一个处理请求 | `Beacon::pick()` |
| 故障转移 | Failover | 请求失败时自动切换到另一个实例重试 | `Beacon::pick(exclude)` |
| 熔断 | Circuit Breaker | 连续失败达到阈值后停止请求，防止雪崩 | beacon 网关侧（Phase 3） |
| 服务网格 | Service Mesh | 基础设施层处理服务间通信（发现/LB/熔断/观测） | beacon 网关 + PHP 扩展 |
| 租约 | Lease | 注册中心给实例的存活凭证，需定期续期 | etcd `LeaseGrant(TTL=15s)` |
| 存活时间 | TTL (Time To Live) | 租约的有效期，过期自动删除 | `beacon.heartbeat_ttl` |
| 监听 | Watch | 注册中心推送实例变更通知（长轮询/流式） | `RegistryAdapter::watch()` |
| 端点 | Endpoint | 服务实例的网络地址（host:port） | `pick()` 返回的 `{host, port}` |
| 实例 | Instance | 服务的一个运行副本（一个 FPM pool = 一个实例） | `Beacon::getInstances()` 返回的数组元素 |
| 注册中心 | Registry | 存储服务实例信息的中心组件 | `RegistryAdapter` SPI |
| 解析器 | Resolver | 将服务名解析为实例列表的组件 | `RegistryAdapter::discover()` |
| 均衡器 | Balancer | 从实例列表中选择一个的组件 | `Beacon::pick()` 内置 LB |
| 选择器 | Picker | 均衡器的具体选择算法实现 | `Beacon::LB_ROUND_ROBIN` 等 |
| 异常检测 | Outlier Detection | 通过实际流量被动观察实例健康（非主动探测） | beacon 网关侧（Phase 3） |
| 降级 | Degraded | 实例饱和/过载，降优先级但不摘除 | `Beacon::HEALTH_DEGRADED` |
| 就绪 | Readiness | 实例是否准备好接收流量 | `Beacon::ready()` / `HEALTH_NOT_READY` |
| 存活 | Liveness | 实例是否还活着（进程级） | 治理 worker 崩溃 → lease 过期 |
| 启动探针 | Startup Probe | 慢启动应用的专属启动检测窗口 | `HEALTH_NOT_READY` → `HEALTH_OK` |
| 优雅关闭 | Graceful Shutdown | 停止前完成进行中的请求，注销实例 | `Beacon::deregister()` + pool stop |
| 反熵 | Anti-entropy | 注册中心与实例间的状态同步机制 | etcd lease + keepalive |
| 共识 | Consensus | 分布式系统达成一致状态的算法 | etcd Raft |
| 自报健康 | Self-reported Health | 实例主动上报健康状态（非被动探活） | `HealthChecker` + `keepalive($id, $health)` |
| 饱和 | Saturation | 实例负载接近上限，应降优先级 | `FpmPoolHealthChecker` 自计数 busy |
| 服务治理 | Service Governance | 管理微服务生命周期的控制面（注册/发现/健康/路由/熔断） | beacon 整体定位 |

---

## 十四、最小实现 MVP 路径

### 14.1 MVP 的核心命题

**最小实现功能闭环**：FPM 启动 → 治理 worker 自注册到注册中心 → 保活（带 FPM 池感知健康）→ 发现 peer 节点 → FPM worker `pick()` 选址 → 调 peer。

**MVP 的边界**：

| 做 | 不做（后续） |
|---|---|
| 注册中心回调注入（PHP 层，不内置） | 内置注册中心实现（Consul/etcd C 层） |
| FPM 池感知健康检查（内置默认） | 业务级 HealthChecker 注入 |
| shm 持久化（内置默认） | Persistence L2（Redis/文件） |
| INI 配置地址（内置默认） | AdvertiseResolver 注入 |
| 方案 A（FPM spawn + fork + exec） | 方案 B（systemd 独立服务） |
| 核心回调（on_keepalive/on_discover） | 可选回调（on_register/on_deregister/on_watch） |
| 单服务（`beacon.service_name`） | 多服务（`Beacon::serveAll` + `Beacon::serve`） |
| 文件模式默认注册中心 | etcd/Consul 适配器 |
| ReactPHP 协程（治理 worker） | C 层 libcurl multi |
| C 结构体 shm（零反序列化） | — |
| per-worker 心跳槽位（64B 对齐） | — |
| 无治理模式语义（governance_alive + 缓存 TTL） | — |

### 14.2 MVP 的实现顺序

按依赖方向，从底层到上层：

```
1. 基础设施层（beacon_shm.c + beacon_config.c）
   ↓
2. 领域服务层（beacon_service_health.c + beacon_service_select.c）
   ↓
3. 生命周期层（beacon_governance_worker.c + beacon_callback.c）
   ↓
4. PHP API 层（beacon_api.c + beacon_api_governance.c）
   ↓
5. 模块入口（beacon.c）
```

### 14.3 MVP 的实现计划

**Phase 1：基础设施（1 周）**

| 文件 | 做什么 | 验收 |
|---|---|---|
| `beacon_config.c` | INI 解析（`beacon.enabled`/`service_name`/`advertise_host` 等 8 个核心项） | `php -i` 能看到 INI 配置 |
| `beacon_shm.c` | sysv shm 双缓冲封装（C 结构体 + 心跳槽位 + CRC 校验） | shm 能存取节点表，双缓冲无锁 |

**Phase 2：领域服务层（1 周）**

| 文件 | 做什么 | 验收 |
|---|---|---|
| `beacon_service_health.c` | 读 shm 自计数 + 心跳槽位校准，算 pool 级健康状态 | 能读 shm 自计数，返回健康状态 |
| `beacon_service_select.c` | 从 shm 读节点，LB 选址（round-robin/random/weighted） | `pick()` 返回健康节点 |

**Phase 3：生命周期层（1 周）**

| 文件 | 做什么 | 验收 |
|---|---|---|
| `beacon_governance_worker.c` | spawn 治理 worker（fork + close_all_fds + prctl + exec） | FPM 启动，治理 worker 自注册到注册中心 |
| `beacon_callback.c` | C 层调 PHP 回调（zend_call_function + 耗时检测） | 回调能执行，耗时超阈值记 warn |

**Phase 4：PHP API 层 + 模块入口（1 周）**

| 文件 | 做什么 | 验收 |
|---|---|---|
| `beacon_api.c` | `Beacon::pick()`/`Beacon::getInstances()`/`Beacon::ready()`/`Beacon::status()` | PHP 代码能调 `Beacon::pick()` |
| `beacon_api_governance.c` | `Beacon\Governance::storeNodes()`/`commit()`（CLI SAPI 专用） | 治理 worker 能写 shm |
| `beacon.c` | MINIT/MSHUTDOWN/RINIT/RSHUTDOWN，注册类/常量/INI | 扩展能加载，常量能访问 |

**总计：4 周**

### 14.4 MVP 的验收标准

**功能验收**：

1. FPM 启动，扩展 spawn 治理 worker（独立进程），worker 自注册到注册中心
2. 治理 worker 每 3s keepalive，携带 FPM 池感知健康
3. 治理 worker 每 2s discover，写 shm
4. FPM worker 调 `Beacon::pick()` 从 shm 读节点
5. 治理 worker 崩溃，master 重启，重新注册

**性能验收**：

- 10k req/s 下自计数开销 < 1ms/s
- `pick()` 延迟 < 1μs
- shm 占用 < 10MB

**稳定性验收**：

- 治理 worker 崩溃后 15s 内重启，实例不被误摘
- etcd 不可达时，FPM worker 用 shm 缓存继续服务

### 14.5 MVP 后的迭代路径

| 迭代 | 内容 | 优先级 |
|---|---|---|
| MVP+1 | 多服务支持（`Beacon::serveAll` + `Beacon::serve`） | 高 |
| MVP+1 | Hooks 完整实现（6 个回调） | 中 |
| MVP+2 | AdvertiseResolver 注入（K8s/云 metadata） | 高 |
| MVP+2 | Persistence L2（Redis/文件，异步持久化） | 中 |
| MVP+3 | Consul/Nacos 适配器 | 中 |
| MVP+3 | C 层 watch（libcurl multi） | 中 |
| MVP+4 | 客户端熔断（reportFailure + consecutive_failures） | 中 |
| MVP+4 | beacon-yar 包（一行调 Yar + 自动重试） | 低 |
| MVP+5 | Prometheus metrics / statsd | 低 |
| 成熟期 | K8s 完整适配（readinessProbe/preStop） | 中 |
| 成熟期 | systemd service 模板 | 低 |

### 14.6 实现要求

**实现原则**：

**垂直、单一、互不干扰、低耦合、高内聚、简单明了。分层、分文件、边界明显。**

| 原则 | 含义 | 落地方式 |
|---|---|---|
| **垂直** | 每个功能点独立成文件，不横向蔓延 | 一个 C 文件管一个功能域 |
| **单一** | 每个文件只做一件事 | `beacon_shm.c` 只管 shm，`beacon_service_select.c` 只管选址 |
| **互不干扰** | 模块间通过接口通信，不直接访问内部状态 | SPI 层定义接口，领域层调接口不碰实现 |
| **低耦合** | 模块间依赖最小化，可独立替换 | 注册中心可换（文件→etcd），不影响其他模块 |
| **高内聚** | 相关功能聚在一起 | 所有 shm 操作在 `beacon_shm.c`，不散落各处 |
| **简单明了** | 代码可读性优先，不过度设计 | 每个文件 < 500 行，函数 < 50 行 |
| **分层** | 依赖方向单向，上层 → 下层 | PHP API → Lifecycle → Domain Service → Adapter → Entity → Infrastructure |
| **分文件** | 一个功能域一个文件 | 见功能模块对应表 |
| **边界明显** | 模块间有清晰的接口边界 | SPI 接口是边界，实现可替换 |

**功能模块对应表**：

| 功能点 | C 文件 | 职责 | 依赖 | 层 |
|---|---|---|---|---|
| 模块入口 | `beacon.c` | MINIT/MSHUTDOWN/RINIT/RSHUTDOWN，注册类/常量/INI，spawn 治理 worker | 所有 | 入口 |
| PHP API | `beacon_api.c` | `Beacon::pick/serve/ready/getInstances/status` 等 PHP 用户态 API | service, shm | API |
| 治理 worker API | `beacon_api_governance.c` | `Beacon\Governance::storeNodes/commit`（CLI SAPI 专用） | shm | API |
| 治理 worker | `beacon_governance_worker.c` | spawn 治理 worker，状态机，主循环，timer 调度 | shm, config | Lifecycle |
| 回调调度器 | `beacon_callback.c` | `zend_call_function` + 耗时检测 + 异常隔离 | — | Lifecycle |
| 健康计算 | `beacon_service_health.c` | 读 shm 自计数 + 心跳槽位校准，算 pool 级健康状态 | shm | Domain Service |
| 节点选取 | `beacon_service_select.c` | 从 shm 读节点，LB 选址（round-robin/random/weighted） | shm | Domain Service |
| 共享内存 | `beacon_shm.c` | sysv shm 双缓冲 + C 结构体 + 心跳槽位 + CRC 校验 | — | Infrastructure |
| 配置 | `beacon_config.c` | INI 解析 + 常量注册 | — | Infrastructure |

**砍掉的模块**（移至 PHP 层）：
- ❌ `beacon_adapter_registry.c` — 注册中心适配器移至 PHP 层（`on_register`/`on_keepalive`/`on_discover`/`on_deregister` 回调）
- ❌ `beacon_adapter_health.c` — HealthChecker 注入移至 PHP 层
- ❌ `beacon_adapter_persistence.c` — Persistence 移至 PHP 层（shm 就是唯一存储）
- ❌ `beacon_adapter_hooks.c` — Hooks 移至 PHP 层
- ❌ `beacon_adapter_advertise.c` — AdvertiseResolver 移至 PHP 层
- ❌ `beacon_lifecycle_watch.c` — watch 合并进 discover 轮询（一期）
- ❌ `beacon_entity_*.c` — 实体简化为 C 结构体，无需独立 C 文件

**C 扩展从原稿的 ~18 个文件精简到 ~8 个文件**，核心代码量从预估 5000 行降到 2000 行以内。

**分层架构**：

```
┌─────────────────────────────────────────┐
│  PHP Userland（业务代码 + 注入包）        │
│  ├─ beacon-consul（默认适配器，composer） │  ← 实现 SPI：on_keepalive / on_discover
│  ├─ 用户自定义适配器（etcd/Nacos/自研）   │
│  └─ 业务代码：Beacon::pick() 消费节点     │
├─────────────────────────────────────────┤
│  PHP API 层（beacon_api.c）              │  ← PHP 用户态调用
│  Beacon::pick() / Beacon::reportHealth() │
│  Beacon::setOpt() / Beacon::configure()  │
├─────────────────────────────────────────┤
│  生命周期层（Lifecycle）                 │  ← 何时做
│  ├─ beacon_governance_worker.c           │     spawn 治理 worker，状态机，主循环
│  └─ beacon_callback.c                    │     C 层调 PHP 回调（zend_try + 耗时检测）
├─────────────────────────────────────────┤
│  领域服务层（Domain Service）            │  ← 做什么
│  ├─ beacon_service_health.c              │     读 shm 自计数 + 校准，算 pool 级健康
│  └─ beacon_service_select.c              │     从 shm 读节点，LB 选址
├─────────────────────────────────────────┤
│  基础设施层（Infrastructure）            │  ← 底层支撑
│  ├─ beacon_shm.c                         │     sysv shm（C 结构体双缓冲 + 心跳槽位）
│  └─ beacon_config.c                      │     INI 解析
├─────────────────────────────────────────┤
│  模块入口（beacon.c）                    │  ← MINIT/MSHUTDOWN/RINIT/RSHUTDOWN
│  MINIT：init shm → spawn worker          │
│  RINIT/RSHUTDOWN：自计数原子操作         │
│  MSHUTDOWN：kill worker → 清理 shm      │
└─────────────────────────────────────────┘
```

**关键变化**：Adapter 层从 C 层消失，变成 PHP userland 的 SPI。C 扩展不再有任何注册中心相关的 C 文件。

**依赖方向**：上层 → 下层，不反向。PHP API 层依赖领域服务层，领域服务层依赖基础设施层。模块入口依赖所有（注册）。

**边界规则**：
- 生命周期层管"何时做"，领域服务层管"做什么"——两者分离，不混淆
- 适配器层在 PHP 层（不在 C 层），C 层只做回调调度（`beacon_callback.c`）
- 实体层简化为 C 结构体，无需独立 C 文件
- 基础设施层（shm/config）被领域服务层依赖，但不依赖上层
- PHP API 层只做参数校验 + 转发到领域服务层，不含业务逻辑
- 治理 worker 是唯一做网络 I/O 的模块（通过 PHP 回调），其他模块零网络

**对标 PHP 扩展惯例**：

| 惯例 | 对标 | beacon 落地 |
|---|---|---|
| 分文件组织 | Swoole（`swoole_server.c`/`swoole_process.c`/`swoole_table.c`） | 一个功能域一个 C 文件 |
| 常量注册 | Yar（`YAR_OPT_*`）/ curl（`CURLOPT_*`） | `Beacon::OPT_*` / `Beacon::LB_*` / `Beacon::HEALTH_*` |
| setOpt 模式 | `Yar_Client::setOpt()` / `curl_setopt()` | `Beacon::setOpt()` |
| register 命名 | `spl_autoload_register()` / `register_shutdown_function()` | `Beacon::registerHealthChecker()`（链式追加） |
| set 命名 | `set_error_handler()` / `set_exception_handler()` | `Beacon::setRegistry()` / `Beacon::setPersistence()`（单一替换） |
| 命名空间 | Swoole（`Swoole\Server`） | `Beacon`（顶级类，不搞子命名空间） |

### 14.7 发布策略

| 优先级 | 渠道 | 用户安装方式 |
|---|---|---|
| P0 | 源码 | `git clone && phpize && ./configure && make install` |
| P1 | GitHub Release 预编译 .so | `curl -fsSL beacon.sh/install | bash` |
| P2 | PECL | `pecl install beacon`（有精力再维护，不主推） |
| P3 | 容器镜像 | `FROM beacon/php:8.2-fpm` |
| P4 | 系统包仓库 | `apt install php8.2-beacon`（后期） |

**源码优先的好处**：
- 早期贡献者能直接编译调试
- 架构无绑定，LoongArch/RISC-V 用户自己编
- 不需要维护复杂的 CI 矩阵

---

## 十五、一句话总结

> **FPM 扩展只做三件事：自计数（RINIT/RSHUTDOWN）、shm 读写（C 结构体）、spawn 治理 worker（fork+exec+prctl）。治理逻辑全在 PHP 脚本（ReactPHP 协程），注册中心全在 PHP 层（文件/etcd/Consul 可注入）。零 ualarm，零 JSON，零裸指针，零内置重试。**

这个架构**既保留了 FPM spawn 的进程关系优势，又避免了 C 层过度设计**，是 PHP-FPM 生态下服务治理的**最小可行且正确**的基底。
