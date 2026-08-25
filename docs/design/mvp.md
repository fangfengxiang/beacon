# 最小实现 MVP 路径

> 本文档描述 php-beacon-extension 的最小实现 MVP 路径、实现要求、迭代计划。

---

## 一、MVP 的核心命题

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

---

## 二、MVP 的实现顺序

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

---

## 三、MVP 的实现计划

### Phase 1：基础设施（1 周）

| 文件 | 做什么 | 验收 |
|---|---|---|
| `beacon_config.c` | INI 解析（`beacon.enabled`/`service_name`/`advertise_host` 等 8 个核心项） | `php -i` 能看到 INI 配置 |
| `beacon_shm.c` | sysv shm 双缓冲封装（C 结构体 + 心跳槽位 + CRC 校验） | shm 能存取节点表，双缓冲无锁 |

### Phase 2：领域服务层（1 周）

| 文件 | 做什么 | 验收 |
|---|---|---|
| `beacon_service_health.c` | 读 shm 自计数 + 心跳槽位校准，算 pool 级健康状态 | 能读 shm 自计数，返回健康状态 |
| `beacon_service_select.c` | 从 shm 读节点，LB 选址（round-robin/random/weighted） | `pick()` 返回健康节点 |

### Phase 3：生命周期层（1 周）

| 文件 | 做什么 | 验收 |
|---|---|---|
| `beacon_governance_worker.c` | spawn 治理 worker（fork + close_all_fds + prctl + exec） | FPM 启动，治理 worker 自注册到注册中心 |
| `beacon_callback.c` | C 层调 PHP 回调（zend_call_function + 耗时检测） | 回调能执行，耗时超阈值记 warn |

### Phase 4：PHP API 层 + 模块入口（1 周）

| 文件 | 做什么 | 验收 |
|---|---|---|
| `beacon_api.c` | `Beacon::pick()`/`Beacon::getInstances()`/`Beacon::ready()`/`Beacon::status()` | PHP 代码能调 `Beacon::pick()` |
| `beacon_api_governance.c` | `Beacon\Governance::storeNodes()`/`commit()`（CLI SAPI 专用） | 治理 worker 能写 shm |
| `beacon.c` | MINIT/MSHUTDOWN/RINIT/RSHUTDOWN，注册类/常量/INI | 扩展能加载，常量能访问 |

**总计：4 周**

---

## 四、MVP 的验收标准

### 4.1 功能验收

1. FPM 启动，扩展 spawn 治理 worker（独立进程），worker 自注册到注册中心
2. 治理 worker 每 3s keepalive，携带 FPM 池感知健康
3. 治理 worker 每 2s discover，写 shm
4. FPM worker 调 `Beacon::pick()` 从 shm 读节点
5. 治理 worker 崩溃，master 重启，重新注册

### 4.2 性能验收

- 10k req/s 下自计数开销 < 1ms/s
- `pick()` 延迟 < 1μs
- shm 占用 < 10MB

### 4.3 稳定性验收

- 治理 worker 崩溃后 15s 内重启，实例不被误摘
- etcd 不可达时，FPM worker 用 shm 缓存继续服务

---

## 五、MVP 后的迭代路径

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

---

## 六、实现要求

### 6.1 实现原则

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

### 6.2 功能模块对应表

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

### 6.3 分层架构

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

### 6.4 对标 PHP 扩展惯例

| 惯例 | 对标 | beacon 落地 |
|---|---|---|
| 分文件组织 | Swoole（`swoole_server.c`/`swoole_process.c`/`swoole_table.c`） | 一个功能域一个 C 文件 |
| 常量注册 | Yar（`YAR_OPT_*`）/ curl（`CURLOPT_*`） | `Beacon::OPT_*` / `Beacon::LB_*` / `Beacon::HEALTH_*` |
| setOpt 模式 | `Yar_Client::setOpt()` / `curl_setopt()` | `Beacon::setOpt()` |
| register 命名 | `spl_autoload_register()` / `register_shutdown_function()` | `Beacon::registerHealthChecker()`（链式追加） |
| set 命名 | `set_error_handler()` / `set_exception_handler()` | `Beacon::setRegistry()` / `Beacon::setPersistence()`（单一替换） |
| 命名空间 | Swoole（`Swoole\Server`） | `Beacon`（顶级类，不搞子命名空间） |

---

## 七、发布策略

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
