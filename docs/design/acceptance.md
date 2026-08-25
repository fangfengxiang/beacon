# 验收标准与取舍边界

> 本文档描述 php-beacon-extension 的验收标准、取舍与边界。

---

## 一、验收标准

### 1.1 功能验收

1. FPM 启动，扩展 spawn 治理 worker（独立进程），worker 自注册到注册中心，beacon 能读到该实例
2. 注入自定义 `HealthChecker`（返回 degraded），keepalive 携带 degraded，beacon 读到并降权路由
3. 注入自定义 `Persistence`（存 Redis），FPM worker `getInstances` 从 shm 读到节点（shm 是 L1，Redis 是 L2）
4. 不注入任何实现，用内置默认（文件模式 + FPM 池感知 + shm），开箱即用
5. 杀治理 worker，master 重启它，重新注册，实例不被误摘（TTL 兜底）
6. 注入的 `health_checker` 抛异常，扩展降级到内置默认健康，不崩 worker

### 1.2 性能验收

| 指标 | 目标 | 测量方式 |
|---|---|---|
| 自计数开销 | 10k req/s 下 < 1ms/s | `ab -n 100000 -c 100` 压测，对比扩展启用前后的 QPS |
| `pick()` 延迟 | < 1μs | 基准测试，shm 读 + 进程内 LB |
| shm 占用 | < 10MB（100 服务 × 10 实例） | `ipcs -m` 查看 shm 段大小 |
| 冷启动 | pool 启动到首次注册 < 1s | 日志时间戳差 |

### 1.3 稳定性验收

| 场景 | 目标 | 验证方式 |
|---|---|---|
| 治理 worker 崩溃 | 15s 内重启，实例不被误摘 | `kill -9` 治理 worker，观察 etcd 实例是否存活 |
| 注册中心不可达 | 用本地缓存继续服务，不注册但能发现已有节点 | 断 etcd 网络，观察 `pick()` 是否正常 |
| 健康检查器全部失败 | 降级为内置默认（FPM 池感知） | 注入的 HealthChecker 全部抛异常 |
| shm 不可用 | 直接查注册中心（慢但能用） | 模拟 shm 损坏 |
| 治理 worker 反复崩溃 | 停止重启，降级为"无治理模式"（FPM 正常服务请求） | 连续 kill 治理 worker 5 次 |

---

## 二、取舍与边界

### 2.1 治理 worker 崩溃

worker 崩溃 = 无保活 = 实例在注册中心 TTL 过期被摘。缓解：
- master 从 shm 读 `governance_pid`，`kill(pid, 0)` 检测，崩溃自动重启（重启后重新 register，幂等）
- 重启窗口内 keepalive 间断，靠注册中心 TTL 兜底（etcd lease TTL 设宽松些，如 15s）
- 极端兜底：master 线程跑一个最简 C 级 keepalive（不依赖 worker，路线 A 兜底），仅续 lease 不调 userland

### 2.2 注入的 PHP 实现崩溃

业务注入的 `health_checker.check()` 抛异常 = keepalive 失败。扩展用 `try/catch`（C 层 `zend_call_function` 包 `try`）隔离，异常时用内置默认健康（FPM 池感知）兜底，不让 userland 崩溃拖垮治理 worker。

### 2.3 IPC 延迟

FPM worker `getInstances` 读 shm 是共享内存直读，无 IPC 调用，纳秒级。watch 推送到 shm 有毫秒级延迟（worker → shm 写 → FPM worker 读），靠 beacon 故障转移兜底（请求失败重试）。

### 2.4 不做的事

- **不做协议转换**：那是 beacon/grpc-yar-bridge 的活
- **不做请求路由/LB**：那是 beacon 网关的活（形态 B 下 PHP 自己 LB，但那是 client 侧负载均衡，不是网关）
- **不做熔断/限流**：可观测性层，留给 beacon Phase 4 或独立组件
- **不绑定 Yar**：服务任意 PHP 服务，Yar 只是消费者之一

### 2.5 完整降级链

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

---

## 三、日志记录点

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

---

## 四、防御性编程

扩展自身异常不影响 FPM：

| 位置 | 风险 | 防御 |
|---|---|---|
| MINIT | 段错误导致 master 崩溃 | 只做最小初始化（注册常量/INI/shm），不做复杂逻辑 |
| RINIT/RSHUTDOWN | 段错误导致 worker 崩溃 | 只做原子操作（shm inc/dec），`zend_try` 包裹，异常不影响请求处理 |
| 治理 worker | 崩溃导致无保活 | master 从 shm 读 `governance_pid`，`kill(pid, 0)` 检测 → 重启 → 重新 register（幂等） |
| 注入的 PHP 实现 | 抛异常拖垮治理 worker | C 层 `zend_call_function` 包 `zend_try` → 降级内置默认 |
