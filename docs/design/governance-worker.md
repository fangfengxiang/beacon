# 治理 Worker：生命周期、spawn、状态机

> 本文档描述 php-beacon-extension 治理 worker 的完整生命周期、spawn 机制、状态机设计。

---

## 一、治理 worker 是什么

治理 worker 是一个**独立的 PHP CLI 进程**，由 FPM master 在 MINIT 阶段 spawn（fork + exec）。它：

- 有完整的 PHP VM，能执行 PHP 回调（注册中心通信、健康检查、服务发现）
- 运行 ReactPHP event loop，协程调度 IO，不阻塞 timer
- 通过 shm 和 FPM worker 通信（写节点表、读自计数）
- 崩溃后由 FPM master 检测并重启

---

## 二、spawn 机制

### 2.1 启动流程

```c
// beacon_governance_worker.c

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

### 2.2 关键机制

| 机制 | 作用 | 为什么必须 |
|------|------|-----------|
| `close_all_fds_except_std()` | 关闭所有非标准 fd | 防止继承 FPM 的 listen socket、数据库连接等 |
| `setsid()` | 脱离控制终端 | 避免 FPM 收到 SIGHUP 时传导给治理 worker |
| `prctl(PR_SET_PDEATHSIG, SIGTERM)` | 父进程退出时内核自动发 SIGTERM | 解决 graceful reload 时旧治理 worker 孤儿化问题 |
| `exec()` | 替换地址空间，启动全新 VM | fork 后 VM 状态已污染，必须 exec 重置 |

### 2.3 fd 清理实现

跨平台目录遍历（Linux `/proc/self/fd`，macOS/BSD `/dev/fd`），失败兜底 `sysconf` 盲扫：

```c
static void close_all_fds_except_std(void) {
#if defined(__linux__)
    const char *fd_dir_path = "/proc/self/fd";
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__sun)
    const char *fd_dir_path = "/dev/fd";
#else
    const char *fd_dir_path = NULL;
#endif

    if (fd_dir_path) {
        DIR *dir = opendir(fd_dir_path);
        if (dir) {
            int dir_fd = dirfd(dir);
            struct dirent *entry;
            int fds[1024];
            int count = 0;

            while ((entry = readdir(dir)) != NULL && count < 1024) {
                int fd = atoi(entry->d_name);
                if (fd > 2 && fd != dir_fd) {
                    fds[count++] = fd;
                }
            }
            closedir(dir);

            for (int i = 0; i < count; i++) {
                close(fds[i]);
            }
            return;
        }
        /* 目录不可用 → 落入盲扫兜底 */
    }

    /* 兜底：POSIX sysconf 盲扫（替代 macOS 已 deprecated 的 getdtablesize） */
    long max_fd = sysconf(_SC_OPEN_MAX);
    for (long fd = 3; fd < max_fd; fd++) {
        close((int)fd);
    }
}
```

> 平台取舍详见文末 ADR-GW-002。

### 2.4 exec 路径探测与环境构造

编译期推导链（config.m4）：`BEACON_PHP_BIN` = 环境变量 > `php-config --php-binary` > `which php` > `/usr/bin/php`；`BEACON_DATA_DIR` = 环境变量 > `php-config --prefix`/share/php/beacon。优先 php-config 推导保证治理 worker 的 PHP 版本与扩展编译版本一致。`make install` 自动安装 governance.php 到 `BEACON_DATA_DIR`。

exec 环境构造（运行期）：子进程是全新 PHP CLI，不继承父进程 `-d` 参数与扩展加载状态。spawn 显式构造 argv：`php -d extension=beacon.so -d beacon.enabled=1 -d <治理相关 INI>... governance.php`。`shm_key` 非 0 时必传（保证 attach 同一段 shm）。详见文末 ADR-GW-003。

```c
static const char* beacon_get_php_bin(void) {
    // 运行时 INI 优先
    if (BEACON_G(governance_bin) && strlen(BEACON_G(governance_bin)) > 0) {
        return BEACON_G(governance_bin);
    }
    // 编译时宏兜底（php-config --php-binary 推导）
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

---

## 三、生命周期状态机

### 3.1 状态定义

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

### 3.2 状态转换图

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

---

## 四、治理 worker 主循环（ReactPHP）

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

---

## 五、治理 worker 崩溃与重启

### 5.1 FPM master 监控

```c
// beacon.c::MINIT

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

### 5.2 治理 worker 自检（getppid 兜底）

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

### 5.3 优雅退出

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

---

## 六、无治理模式

### 6.1 定义

治理 worker 反复崩溃（连续 5 次重启失败）→ 进入"无治理模式"：

| 状态 | 触发条件 | FPM 行为 | 治理行为 |
|------|---------|---------|---------|
| FULL | 治理 worker 正常 | pick() 读 shm，正常服务 | 注册/保活/发现 |
| DEGRADED | 治理 worker 崩溃，但 shm 有效 | pick() 读 shm，标记陈旧 | 无，等重启 |
| NONE | 治理 worker 反复崩溃，或 shm 损坏 | pick() 返回空 / 异常 | 无 |

### 6.2 缓存 TTL

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

### 6.3 SRE 可观测性

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

---

## 七、关键设计决策

| 决策 | 理由 |
|------|------|
| **FPM spawn 而非 systemd** | 进程关系带来 prctl/SIGCHLD 能力，开箱即用 |
| **fork + exec 而非纯 fork** | PHP VM 单例限制，fork 后必须 exec 重置 VM |
| **ReactPHP 而非 C 层 libcurl** | C 层保持极简，PHP 层自治超时，不引入 libcurl 依赖 |
| **prctl + getppid 双保险** | 内核级 + 用户态兜底，解决 graceful reload 孤儿问题 |
| **无治理模式语义** | 缓存 TTL + Beacon::status()，SRE 可观测 |

---

## 八、实现期 ADR

### ADR-GW-001：编译期路径从 php-config 推导（BEACON_PHP_BIN / BEACON_DATA_DIR）

**状态**：已实施（2026-08-26）

**背景**：初版 config.m4 硬编码 `/usr/bin/php` 与 `/usr/share/php/beacon`（FHS 假设）。Homebrew（`/opt/homebrew`）、源码编译（`/usr/local`）、多版本共存场景下全部错误——macOS 上 `/usr/bin/php` 甚至不存在，治理 worker exec 直接失败。且 `BEACON_PHP_BIN` exec 出的 PHP 必须与加载 beacon.so 的 PHP 同 API 版本，硬编码路径无法保证。

**决策**：编译期推导链——
- `BEACON_PHP_BIN`：环境变量 `PHP_BEACON_PHP_BIN` > `php-config --php-binary` > `which php` > `/usr/bin/php`
- `BEACON_DATA_DIR`：环境变量 `PHP_BEACON_DATA_DIR` > `php-config --prefix`/share/php/beacon（对齐 PEAR share/php/pear 惯例）
- `make install` 通过 Makefile.frag 的 `install: install-beacon-data`（无 recipe 依赖合并钩子）安装 governance.php

**驱动因素**：
1. 版本一致性：phpize 构建时 `$PHP_CONFIG` 指向的正是扩展对齐的 PHP 安装，推导优于假设
2. 防护：老 php-config 不支持 `--php-binary` 时 usage 输出到 stdout，用 `test -x` 拦截污染
3. 运行时仍可由 `beacon.governance_bin` / `beacon.governance_script` INI 覆盖，编译期与运行期职责分层

**业界参考**：ext/opcache、ext/xdebug 均以 `$PHP_CONFIG` 推导路径；`php-config --php-binary` 自 PHP 5.4 起可用；ext/phar Makefile.frag 安装 phar.phar 用同样的 install 钩子机制。

**代码评价**：四级兜底链闭合，configure 输出 `beacon: php binary = ...` 可观测；Makefile.frag 无 recipe 依赖合并不侵入 PHP 构建系统的 install recipe。

### ADR-GW-002：fd 清理跨平台——/proc/self/fd 与 /dev/fd 目录遍历 + sysconf 兜底

**状态**：已实施（2026-08-26）

**背景**：初版 `close_all_fds_except_std()` 用 `#ifdef HAVE_PRCTL` 守卫 `/proc/self/fd` 路径——prctl 可用性与 /proc 可用性是两回事，仅恰好在 Linux 共存，语义错误的守卫是维护陷阱。macOS 分支用 `getdtablesize()`，该 API 在 macOS SDK 已标记 `__POSIX_C_DEPRECATED(199506L)`。

**决策**：平台目录参数化——
- Linux：`/proc/self/fd`（procfs）
- macOS/BSD/Solaris：`/dev/fd`（fdesc 默认挂载）
- 兜底：`sysconf(_SC_OPEN_MAX)` 盲扫（目录不可用或未知平台）

**驱动因素**：
1. macOS 无 `closefrom()`（全 SDK 头文件实测无声明，该函数是 FreeBSD/Solaris/glibc 2.34+ 特有）
2. macOS 有 `/dev/fd`（fdesc 默认挂载，实测可用），与 Linux 共用目录遍历逻辑，仅路径不同
3. `sysconf(_SC_OPEN_MAX)` 是 POSIX 标准，替代 deprecated 的 `getdtablesize()`；`close(EBADF)` 开销纳秒级，盲扫无害

**业界参考**：libuv `uv__process_close_fds`（Linux /proc/self/fd + 其他平台 sysconf 循环）；OpenSSH closefrom fallback 循环。PHP 核心/Swoole/nginx 靠 `FD_CLOEXEC` 预防不做全量关闭，但 FPM listen socket 故意不带 CLOEXEC（要传给 worker accept），beacon 必须主动关。

**代码评价**：守卫从"能力探测"（HAVE_PRCTL）改为"平台识别"（`__linux__`/`__APPLE__`），语义正确；目录遍历失败自动落入盲扫，无新增失败模式；`fds[1024]` 上限对 FPM master 足够，超出部分由 FD_CLOEXEC 兜底。

### ADR-GW-003：spawn 环境构造——execv 显式传递扩展加载与治理 INI

**状态**：已实施（2026-08-26）

**背景**：本机实测发现治理 worker 启动即退出（`status=1`，报 `beacon.service_name not configured`）。根因：exec 的子进程是全新 PHP CLI，**不继承**父进程的 `-d` 命令行参数，系统 php.ini 也未必加载 beacon.so——而 governance.php 依赖 `Beacon\Governance` 类 API（calcHealth/storeNodes/commit）与 `beacon.*` INI。此前 `/usr/bin/php` 硬编码导致 exec 直接失败，该问题被掩盖；ADR-GW-001 修好路径后才暴露。

**决策**：spawn 从 `execl(php, script)` 改为 `execv` + 显式 argv 构造——
- `-d extension=beacon.so`：子进程加载扩展（与父进程同一 PHP 安装，extension-dir 相同；php.ini 已加载时重复加载仅 warning）
- `-d beacon.enabled=1`：子进程 MINIT 初始化 shm 的前提
- 字符串 INI 非空才传（service_name/advertise_*/registry_endpoint/lb_strategy/log_*）
- 数值 INI 总是传（keepalive_interval/pull_interval/heartbeat_ttl/health_dead_threshold）
- `shm_key` 非 0 必传：否则子进程 ftok 生成的 key 与父进程显式配置不同，attach 不到同一段 shm

**驱动因素**：
1. 显式环境构造优于隐式继承——治理 worker 行为完全由父进程配置决定，不受系统 php.ini 差异影响
2. shm_key 一致性是硬约束（跨进程共享同一段 shm）
3. 实测驱动：修复后实测验证子进程 `shm attached (key=0x630ee78b, existing)`、keepalive 循环运行、SIGTERM 优雅退出 status=0

**业界参考**：systemd `Environment=`、Docker `-e`——显式环境传递是进程 spawn 的通用实践；Swoole TaskWorker 用纯 fork 继承全部状态（beacon 因 PHP VM 单例限制必须 exec，故需显式构造）。

**顺带修复**（同次实测暴露）：
1. governance.php `getppid()` → `posix_getppid()`（PHP 函数名错误，macOS 无 prctl 时父进程检测是必需路径；实测验证 `parent changed (43856 -> 1), exiting` 正确触发）
2. governance.php `extension_loaded('beacon')` 前置检查（未加载友好退出而非 fatal error）
3. `Beacon\Governance::calcHealth()` 补治理 worker 心跳更新（`shm->header.governance_alive = time(NULL)`）——原实现只在 spawn 时写一次心跳，15s（heartbeat_ttl）后 FPM 读侧误判 `governance_alive=false`。修复点选 calcHealth 而非 beacon_health_calculate：后者也被 FPM worker 读侧调用，读侧不应写治理心跳；calcHealth 是治理 worker 专属 API，调用即心跳
4. `registry_endpoint` 支持 `file://` 前缀指定 FileRegistry 路径（如 `file:///tmp/beacon-registry`）——macOS 开发、容器等 `/var/run/beacon` 不可写场景。空值仍默认 `/var/run/beacon`，语义向后兼容

**代码评价**：argv 构造集中在 `beacon_build_governance_argv()`，容量上限 40 参数/16 INI 缓冲有编译期余量；`BEACON_GOVERNANCE_WORKER=1` 环境变量 guard 与 INI 传递正交（防 fork 循环 vs 环境构造），职责不混。
