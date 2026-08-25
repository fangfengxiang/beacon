# php-beacon-extension 设计评估与取舍更新

> 来源：Kimi 技术评审报告（逐章 battle 讨论）
> 评估视角：lua-resty-php-beacon 项目定位（OpenResty 上的 PHP 微服务生态完善层）
> 日期：2026-08-23

---

## 一、评估总览

Kimi 报告对 `php-beacon-extension` 设计文档进行了逐章技术评审，覆盖 C 扩展工程、FPM 进程模型、微服务治理语义、Go 工程实践、高性能、高可用、SRE 极简主义、调用方/使用方视角等 9 大维度、20+ 个子问题。

**核心结论：报告的技术判断基本正确，且与项目定位高度一致。** 主要修正是将设计从"理论正确但工程危险"拉回到"工程可行且极简"。

### 关键修正汇总

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

---

## 二、逐项评估

### 2.1 FPM spawn 模式保留（1.1 fd 继承）

**Kimi 判断**：FPM spawn 模式不能放弃（进程关系带来的状态感知能力不可替代），但必须解决 fd 继承问题。

**项目定位评估**：✅ 正确。beacon 扩展的核心价值是"PHP 自己亮灯"——FPM master spawn 治理 worker 是实现这一价值的最自然方式。systemd 外置方案虽然更干净，但失去了进程关系带来的 `prctl(PR_SET_PDEATHSIG)` 和 `SIGCHLD` 精准监控能力。

**取舍**：
- 保留 FPM spawn，子进程 exec 前执行 `close_all_fds()`（遍历 `/proc/self/fd` 关闭非标准 fd）
- 加 `prctl(PR_SET_PDEATHSIG, SIGTERM)` 解决 graceful reload 孤儿问题
- 加 `getppid()` 用户态自检作为兜底
- 二期可选 systemd 模式（企业内网有 SRE 基础设施时）

### 2.2 删除 ualarm（1.2 信号不安全）

**Kimi 判断**：`ualarm` + `zend_try` 是文档中最危险的设计。SIGALRM 异步信号打断 PHP 内存分配，可能导致 Zend MM 堆状态不一致、segfault。

**项目定位评估**：✅ 正确。beacon 扩展的核心原则是"扩展的任何异常都不应导致 FPM 无法服务请求"。ualarm 引发的 segfault 会直接违反这一原则。

**取舍**：
- 彻底删除 C 层超时机制
- PHP 层自治超时：治理 worker 用 ReactPHP 协程调度 IO，HTTP 请求自带 `CURLOPT_TIMEOUT_MS`
- C 层只做"善意提示"：记录回调耗时，超阈值记 warn，但不中断

### 2.3 C 结构体替代 JSON（1.4 性能）

**Kimi 判断**：JSON 序列化是性能瓶颈。每次 `pick()` decode JSON 开销 0.1-1ms，一个请求调 5-10 个下游服务 = 1-10ms 额外延迟。

**项目定位评估**：✅ 正确。beacon 扩展的核心性能指标是 `pick()` 延迟 < 1μs。JSON decode 无法满足这一指标。

**取舍**：
- 一期直接上 C 结构体数组（packed binary），废除 JSON
- shm 布局：header（256B）+ 双缓冲（2 × 16 services × 64 nodes × 392B ≈ 800KB）
- 治理 worker 通过 C 扩展 API（`Beacon\Governance::storeNodes()`）把 PHP 数组转为 C 结构体写入 shm
- FPM worker 直接指针偏移读 C 结构体，零反序列化、零内存分配

### 2.4 per-worker 心跳槽位（2.1 自计数泄漏）

**Kimi 判断**：FPM worker 被 kill -9 / OOM / segfault 时，RSHUTDOWN 不执行，pool_busy 永久泄漏。这是健康检查准确性的根基。

**项目定位评估**：✅ 正确。自计数是 beacon 扩展的核心内置算法，如果计数器不可信，整个"自报健康"体系就是建立在流沙上的。

**取舍**：
- shm header 中增加 `beacon_worker_slot_t workers[256]` 数组
- 每个 slot：pid + last_rinit 时间戳 + busy 标志，64 字节对齐（防 false sharing）
- RINIT：按 pid % 256 哈希找槽位，写 pid + time + busy=1
- RSHUTDOWN：找槽位，busy=0
- 治理 worker 每 keepalive 前扫描槽位，`kill(pid, 0)` 检测死进程，自动清零

### 2.5 文件模式作为默认注册中心

**Kimi 判断**：MVP 默认注册中心从 etcd 降级为本地文件（`/var/run/beacon/`），零依赖、零配置、调试成本极低。

**项目定位评估**：✅ 正确且极具洞察力。beacon 扩展的定位是"状态信标与治理调度基底"，不是"注册中心客户端"。文件模式完美契合这一定位：
- 零外部依赖（不需要部署 etcd 集群）
- 调试成本极低（`tail -f /var/run/beacon/calc.json`）
- 天然 L2 持久化（治理 worker 重启后从文件恢复 shm）
- 后续升级 etcd 只需换适配器，接口不变

**取舍**：
- 默认注册中心 = 本地文件（FileRegistry）
- 每个实例一个 JSON 文件（`/var/run/beacon/{service}-{id}.json`），无并发写冲突
- 文件写 `updated_at`，治理 worker 启动时清理过期文件（> 60s）
- 切换到 etcd/Consul 只需换 `beacon.governance_script` 指向自定义脚本

### 2.6 INI 唯一配置源（7.1 配置层级）

**Kimi 判断**：INI 13 个 + configure() 12 个 OPT + bootstrap.php 代码注入 = 三层配置源，凌晨 3 点故障时 SRE 要翻三个地方才能定位。

**项目定位评估**：✅ 正确。beacon 扩展的设计原则之一是"配置化是地基"。三层配置源违背了这一原则。

**取舍**：
- INI 是唯一配置源，8 个核心项
- 砍掉 `configure()` 数组（冗余）
- `bootstrap.php` 只用于代码逻辑注入（自定义适配器），不用于配置
- 零配置模式：只配 `beacon.enabled = 1` + `beacon.service_name = "calc"` 就能跑

### 2.7 不注册异常类（8.2 异常体系）

**Kimi 判断**：绝大多数 PHP 扩展（Redis、Memcached、curl）都不注册自定义异常。beacon 不是框架，是基础设施扩展，应该保持简单。

**项目定位评估**：✅ 正确。beacon 扩展的错误处理应遵循 PHP 扩展惯例：用返回值表示状态，不用异常表示状态。

**取舍**：
- 一期不注册异常类
- `pick()` 返回 `?array`，null = 无可用节点（类比 Redis `get` 返回 false）
- `getInstances()` 返回 `array`，空数组 = 无节点
- 扩展内部错误用 `php_error_docref` 写日志，返回 false/null
- 二期可选注册 `Beacon\NoAvailableNodeException`（`pickOrFail()`）

### 2.8 ReactPHP 协程调度（5.3 回调阻塞）

**Kimi 判断**：PHP 非线程安全，C 层无法在不破坏 VM 的前提下中断 PHP 回调。治理 worker 的 PHP 回调阻塞主循环是 PHP 语言模型的固有限制。

**项目定位评估**：✅ 正确。beacon 扩展的 C 层应该极简，治理逻辑全在 PHP 层。ReactPHP 是 PHP 生态中最成熟的异步 IO 库。

**取舍**：
- 治理 worker 用 ReactPHP event loop 调度 IO
- C 层只提供同步 API（`storeNodes()` / `commit()`），不碰网络
- PHP 回调只做内存操作（数据转换、鉴权、格式化），必须 < 100ms
- 文件模式（默认）不需要 ReactPHP（纯同步，零网络）

### 2.9 无治理模式语义（6.1）

**Kimi 判断**：文档提到"连续 5 次重启失败 → 降级为无治理模式"，但未定义 FPM worker 在无治理模式下的行为。

**项目定位评估**：✅ 正确。beacon 扩展的核心原则是"扩展的任何异常都不应导致 FPM 无法服务请求"。无治理模式的精确定义是这一原则的体现。

**取舍**：
- shm header 增加 `governance_alive` 时间戳（治理 worker 每次写 shm 时更新）
- FPM worker RINIT 轻量检测：30s 未更新 → 记 warn
- `pick()` 缓存 TTL：60s 内正常返回，60-300s 返回但标记 `_stale`，> 300s 返回 null
- 新增 `Beacon::status()` API，供 health check endpoint 调用

### 2.10 双缓冲互救（6.2 shm 损坏）

**Kimi 判断**：双缓冲 CRC 都失败 + 注册中心不可达 = 服务中断。但文件模式下这个问题被大幅缓解（文件在本地，不存在"网络不可达"）。

**项目定位评估**：✅ 正确。文件模式作为默认注册中心，天然解决了"注册中心不可达"的问题。

**取舍**：
- 双缓冲互救：激活 buffer 坏 → 读备份 buffer
- 文件模式恢复：治理 worker 重启后从文件恢复 shm
- `pick()` 返回 NULL 时，业务代码必须处理（文档强制要求）
- 二期可选 L2 持久化（Redis/文件快照）

---

## 三、不采纳的建议

| 建议 | 不采纳理由 |
|------|-----------|
| C 层内置 libcurl multi 做异步 IO | 引入 libcurl 依赖，构建复杂。ReactPHP 已解决异步问题，C 层保持极简 |
| 治理 worker 纯 C 实现（不 exec PHP） | 违背 SPI 注入的核心设计原则。没有 VM 就不能跑 PHP 回调，扩展失去灵活性 |
| 一期内置 Consul 客户端 | 绑定注册中心，违背"不绑定注册中心"的核心设计原则 |
| FPM master 主循环支持 UNIX socket IPC | FPM master 主循环是封闭的（阻塞在 waitpid），扩展无法修改 |
| 扩展 fork 子进程异步回调 PHP 函数 | PHP VM 不是 fork-safe 的，子进程继承 VM 状态后不能再初始化 |

---

## 四、发布策略

**Kimi 建议**：PECL 已边缘化，不要以 PECL 为主发布渠道。

**项目定位评估**：✅ 正确。但优先级需要调整。

**取舍**（用户确认）：
- P0：源码（config.m4 写好探测逻辑，README 写清 phpize && make install）
- P1：GitHub Release 预编译 .so（CI 构建矩阵）
- P2：PECL（有精力再维护，不主推）
- P3：容器镜像（Docker Hub）
- P4：系统包仓库（Launchpad / Copr / Alpine aports）

---

## 五、对原始设计文档的影响

Kimi 报告的修正意味着原始设计文档（`php-beacon-extension-design.md`）需要大幅更新：

1. **架构简化**：C 扩展从 ~18 个文件精简到 ~8 个文件，核心代码量从 5000 行降到 2000 行以内
2. **SPI 变化**：从 C 层接口改为 PHP 回调注入，但回调签名和分级需要更新
3. **shm 设计**：从 JSON 双缓冲改为 C 结构体双缓冲 + per-worker 心跳槽位
4. **治理 worker**：从纯 C 层 timer 循环改为 ReactPHP event loop
5. **配置精简**：INI 从 13 个砍到 8 个，砍掉 configure() 数组
6. **API 精简**：砍掉异常类注册，pick() 返回 ?array，getInstances() 返回 array
7. **默认注册中心**：从 etcd 改为本地文件

这些更新已反映在 `docs/beacon/` 目录下的拆分文档中。

---

## 六、核心原则验证

Kimi 报告的修正与 beacon 扩展的核心原则完全一致：

| 原则 | 修正体现 |
|------|---------|
| 扩展的任何异常都不应导致 FPM 无法服务请求 | 删除 ualarm、per-worker 心跳槽位校准、双缓冲互救、无治理模式语义 |
| 请求路径零 I/O | C 结构体数组（零反序列化）、shm 直读（零网络） |
| 不绑定注册中心、不绑定协议、不绑定存储 | 文件模式默认、PHP 回调注入、ReactPHP 协程 |
| 配置化是地基 | INI 唯一配置源、8 个核心项、零配置模式 |
| 极简 | C 扩展 ~8 个文件 < 2000 行、无内置重试、无异常类 |

**结论：Kimi 报告的修正使设计从"理论正确但工程危险"变为"工程可行且极简"，与项目定位高度一致。**
