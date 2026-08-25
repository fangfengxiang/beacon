# 架构：治理 worker 进程

> 本文档描述 php-beacon-extension 的核心架构：治理 worker 进程模型、IPC 机制、运行原理与数据流。

---

## 一、为什么需要 worker 进程

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

---

## 二、worker 生命周期

- **启动**：FPM pool init 时 fork，调注入的 `on_start` 钩子（注册 self、起 timer）
- **运行**：循环跑 keepalive/pull/watch，调注入的 PHP 函数
- **崩溃**：master 监控，崩溃重启（重启后重新注册，幂等）
- **停止**：FPM pool stop 时调注入的 `on_stop` 钩子（deregister），kill worker

---

## 三、IPC：worker → FPM worker

FPM worker 调 `Beacon::getInstances($service)` 时，不直接调治理 worker（跨进程调用慢）。治理 worker 把节点表写进 **sysv shm**，FPM worker 从 shm 读（共享内存，快）。

```
governance worker ──写──> sysv shm（节点表，C 结构体）<──读── FPM worker
```

shm 是默认持久化存储；若业务注入了别的持久化（如 Redis），shm 降级为 L1 缓存。

---

## 四、治理 worker 的部署方案

### 4.1 方案选择：FPM spawn（保留进程关系）

**业界对标**：New Relic PHP Agent 的 daemon 是**独立可执行文件**（`/usr/bin/newrelic-daemon`），由 PHP 进程 spawn 启动，启动后脱离 PHP 进程树。Datadog 的 trace agent 也是独立进程。

**beacon 的治理 worker 部署方案**：

| 方案 | 做法 | 优点 | 缺点 | 推荐 |
|---|---|---|---|---|
| **A：FPM master fork + exec** | 治理 worker 是 FPM master fork 的子进程，exec PHP 脚本 | 扩展自带，零部署；进程关系带来 prctl/SIGCHLD 能力 | 需要 fd 清理 | ✅ 推荐 |
| **B：systemd 独立服务** | 治理 worker 是独立 PHP 脚本，由 systemd 启动 | 天然独立，不受 FPM 进程树影响 | 需要额外部署；失去进程关系 | 二期可选 |

**为什么方案 A 推荐**：

1. **进程关系价值**：`prctl(PR_SET_PDEATHSIG, SIGTERM)` 让内核在 FPM master 退出时自动通知治理 worker，解决 graceful reload 孤儿问题
2. **SIGCHLD 精准监控**：FPM master 可以精准感知治理 worker 崩溃，毫秒级重启
3. **开箱即用**：装扩展即用，不需要配 systemd

### 4.2 fd 清理（必须）

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

### 4.3 完整 spawn 实现

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

## 六、与 beacon 网关的协同

### 6.1 共享 etcd，职责分离

扩展和 beacon **用同一个 etcd**，但职责不重叠：

| 角色 | 职责 | 对 etcd |
|---|---|---|
| php-beacon-extension | PHP 自注册 + 保活（带自报健康）+ 本地节点缓存 | **写**（register/keepalive）+ 读（discover） |
| beacon 网关 | 路由/LB/故障转移/协议转化 | **读**（读 PHP 自注册的状态做路由决策） |

### 6.2 自注册 > 主动探活

扩展让 PHP 自报健康（master 视角 + 业务级），keepalive 携带权威健康数据。这**彻底消灭 beacon 主动探活的盲区**：
- 无 in-band 排队盲区（PHP 自己报，不用 beacon 探）
- 无网络分区误判（PHP 自己知道活着，不靠 beacon 猜）
- 无饱和检测延迟（PHP 实时报池状态，不等 3s 探活轮）

beacon 从"猜 PHP 健不健康"升级为"读 PHP 自报的健康"。

### 6.3 两种部署形态

**形态 A：扩展 + beacon 共存**（推荐）
- PHP 自注册到 etcd，beacon 从 etcd 读做路由
- 扩展消灭探活盲区，beacon 做网关层路由/LB/协议转化
- 两者协同，各取所长

**形态 B：仅扩展，无 beacon**
- PHP 自注册 + 自发现 + 本地缓存，PHP 直接调 peer（Yar client → peer Yar server）
- 纯 Go 模型，beacon 退化为只做协议转化（gRPC↔Yar）+ ingress
- 适合 PHP 间直连、不需要网关层路由的场景
