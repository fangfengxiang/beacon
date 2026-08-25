# 业界对标与设计溯源

> 本文档基于对 PHP 扩展生命周期、Go 生态服务发现、gRPC/K8s/Consul/Envoy 健康检查的调研，总结 beacon 扩展的设计选择与业界的对应关系，以及值得借鉴的设计思想。

---

## 一、健康检查：应用层自省 vs 传输层探测

**业界共识：连接可达 ≠ 服务可用。**

gRPC Health Checking Protocol（`grpc.health.v1`）的核心设计：健康检查本身是一个标准 gRPC 服务（`Check` + `Watch`），服务端**自主**决定 `SERVING`/`NOT_SERVING`——基于应用层状态（DB 连接池、缓存预热、配置加载），不是 TCP 探测。文档原文：*"A server may choose to reply 'unhealthy' because it is not ready to take requests, it is shutting down or some other reason."*

K8s 三层探针进一步细化：`startup`（启动完成？）→ `liveness`（还活着？）→ `readiness`（能接流量？）。`readiness` 失败**不杀容器**，只移出 Endpoints（停止流量）——优雅处理临时不可用，不重启。

Envoy 的 `degraded` 状态：返回 `x-envoy-degraded` 头的主机**仍参与 LB 但降优先级**，仅在健康主机不足时接流量。饱和不是摘除，是降级——这是业界共识。

**beacon 对标**：自计数健康 = 应用层自省（RINIT/RSHUTDOWN 数 busy/idle，治理 worker 读 shm 报告），三态（ok/degraded/dead）= gRPC SERVING/—/NOT_SERVING + K8s passing/warning/critical + Envoy healthy/degraded/unhealthy。degraded 降优先级 = Envoy 共识。

---

## 二、服务发现：抽象接口 + 可插拔后端

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

---

## 三、进程模型：master 无 VM → fork 有 VM 子进程

**Swoole** 的 Master-Worker 模型：Master 进程纯 C（epoll/kqueue 事件循环，无 PHP VM），fork 出 Worker（有独立 VM）+ TaskWorker（后台任务进程）。IPC 三种：管道（socketpair）、共享内存（`Swoole\Table`/`Swoole\Atomic`）、消息队列（sysvmsg）。Master 崩溃不影响已建立的连接，Worker 崩溃 Master 自动重启。

**PHP-FPM** 的进程模型：master 管理（`fpm_children_create` fork workers），worker 有 VM 处理请求。master 无 VM（不执行 userland）。扩展只有 MINIT/RINIT/RSHUTDOWN/MSHUTDOWN 钩子，MINIT 在 master 执行（fork workers 前），RINIT/RSHUTDOWN 在 worker 执行（per-request）。

**beacon 对标**：治理 worker = Swoole TaskWorker（独立进程，有 VM，做后台工作）。sysv shm = Swoole Table/Atomic（跨进程共享，无锁）。MINIT spawn 治理 worker = Swoole master fork TaskWorker。关键约束一致：master 无 VM → 后台工作必须在独立进程里做。

---

## 四、注册中心：lease/TTL = 健康代理

**etcd lease**：`LeaseGrant(TTL=15s)` + `Put(key, value, lease)`。keepalive 续期 → lease 存活 → key 存在。不续期 → lease 过期 → etcd 自动删 key。lease TTL = 健康代理，无需主动 delete。

**Consul**：`DeregisterCriticalServiceAfter` 字段——服务 critical 状态超过该时间自动注销。agent 定期执行检查（HTTP/TCP/Script），聚合状态 passing/warning/critical。

**K8s**：`readinessProbe` 失败 → Pod 移出 Endpoints → Service 不转发流量。probe 恢复 → Pod 重新加入 Endpoints。

**beacon 对标**：etcd lease TTL(15s) = 健康代理。keepalive 携带健康数据（自报，不是探活）。治理 worker 崩溃 → 不续期 → lease 过期 → etcd 自动删 health key → beacon 摘除该实例。三层兜底：主动 deregister（pool stop）> lease TTL 过期（治理 worker 崩溃）> beacon 侧探活（最后防线）。

---

## 五、启动就绪：startup 窗口

**K8s startup probe**：为慢启动应用提供专属启动检测窗口。`startupProbe` 成功一次后，检测权交给 `liveness`/`readiness`。启动期间不杀容器（`failureThreshold × periodSeconds` 的宽限窗口）。

**beacon 对标**：注册时初始状态 `not_ready`（不接流量），预热完成（`Beacon::ready()` 或首次 keepalive 成功）后转 `ok`。避免"注册了但还没准备好"的窗口——对标 K8s startup probe 的思路。

---

## 六、值得看的文档

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

---

## 七、可借鉴的优秀设计思路

1. **健康检查是应用层自省，不是传输层探测**（gRPC Health）——服务端自主决定 SERVING/NOT_SERVING，比 TCP 探测语义丰富。beacon 自计数正是此思路。

2. **degraded 降优先级，不摘除**（Envoy）——饱和的节点还在处理，只是少分配流量。摘除会导致雪崩（流量转移到其他节点）。beacon 三态路由 ok 优先 degraded 次之。

3. **readiness 失败不杀容器**（K8s）——临时不可用（加载配置、等外部服务）不应重启，只隔离流量。beacon 的 not_ready → ok 状态转换正是此思路。

4. **抽象接口 + 可插拔后端**（go-kit sd）——`Instancer`/`Registrar` 统一接口，各注册中心独立实现。beacon 的 `RegistryAdapter` SPI 正是此思路。

5. **连接建立与发现解耦**（go-kit Factory）——发现返回地址，连接由调用方管。beacon 的 `pick()` 返回节点信息，Yar_Client 自管连接池。

6. **lease/TTL = 健康代理**（etcd）——不续期自动删 key，无需主动 delete。三层兜底（主动 deregister > TTL 过期 > 探活）。

7. **指数退避恢复**（Envoy）——驱逐时间 = base × 连续驱逐次数，恢复后逐步减。避免反复抖动。beacon 网关侧可借鉴。

8. **master 无 VM → fork 有 VM 子进程**（Swoole）——后台工作不能在 master 做（无 VM），fork 子进程（有 VM）做。beacon 治理 worker 正是此思路。

---

## 八、最佳工程实践

1. **SPI + 依赖注入**：扩展定义语义接口（`RegistryAdapter`/`HealthChecker`/`Persistence`/`Hooks`/`AdvertiseResolver`），内置默认实现，允许业务层注入。不强绑定注册中心——对标 go-kit 的可插拔后端、Java SPI（Service Provider Interface）。

2. **请求路径零 I/O**：FPM worker 请求路径只碰 shm（原子操作 + 读），所有注册中心 I/O 在治理 worker。10k req/s 下自计数总开销 ≈ 0.4ms/s，可忽略。对标 Envoy 的数据面零阻塞。

3. **进程隔离**：治理 worker 和 FPM worker 是独立进程——worker 崩不影响治理（治理继续保活），治理崩不影响 worker（worker 仍服务请求，节点缓存陈旧直到重启）。对标 Swoole 的进程隔离。

4. **幂等注册**：治理 worker 崩溃重启后重新 register，同 service+id 覆盖不产生重复。对标 Consul `replace-existing-checks`。

5. **三层兜底**：主动 deregister（pool stop）> lease TTL 过期（治理 worker 崩溃）> beacon 侧探活（最后防线）。任何一层失效，下一层接住。

6. **声明与执行分离**：PHP 入口文件 `Beacon::serve()` 只声明（写 shm），治理 worker 执行注册（调 RegistryAdapter）。声明在 handler 附近，执行在后台——不干扰请求路径。

---

## 九、领域思想美学

**"服务知道自己活着"**——这是 beacon 的核心哲学。传统探活是"猜"（beacon 探 PHP，PHP 被动响应），自注册是"说"（PHP 主动报 beacon，beacon 被动听）。自报消灭探活盲区：in-band 排队、网络分区假阴性、饱和检测延迟——这些探活猜不到的，自报知道。

**"连接可达 ≠ 服务可用"**（gRPC Health）——TCP 连通不代表应用准备好处理请求。应用层自省（DB 连了吗？缓存热了吗？依赖服务通吗？）比传输层探测语义丰富。beacon 的 `HealthChecker` SPI 允许业务注入应用级健康判断。

**"饱和不是死亡"**（Envoy degraded）——busy 率高的 FPM 还在处理，只是该少分配流量。摘除会导致流量转移到其他节点，可能引发雪崩。降优先级是更优雅的处理——degraded 仍参与 LB，只是排在 ok 后面。

**"声明在源头，执行在后台"**——服务声明在 handler 入口文件（`Beacon::serve`），注册执行在治理 worker。声明与 handler 同源（改入口即改声明），执行脱离请求路径（不干扰 worker）。这是关注点分离的体现。

**"lease 是健康代理"**（etcd）——lease TTL = 健康的代理人。keepalive 续期 = 健康，不续期 = 死亡，etcd 自动清理。不需要主动 delete，不需要探活轮询——lease 机制把健康检查简化为"续期 or 不续期"的二选一。

---

## 十、服务治理领域概念词汇表

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
