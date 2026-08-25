# SPI：PHP 回调注入

> 本文档描述 php-beacon-extension 的 SPI（Service Provider Interface）设计，包括注册中心回调、健康检查器、持久化、钩子、地址解析器。

---

## 一、核心变化

SPI 从 C 层接口改为 PHP 回调。C 层只做回调调度（`zend_call_function` + `zend_try`），不内置任何注册中心实现。

**关键约束**：
- C 层不做超时（ualarm 已删除，信号不安全）
- PHP 层自治超时（ReactPHP 协程，HTTP 客户端自带 `CURLOPT_TIMEOUT_MS`）
- C 层只做"善意提示"：记录回调耗时，超阈值记 warn，但不中断

---

## 二、注册中心回调（Registry Callbacks）

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

### 2.1 回调分级

| 回调 | 是否必须 | 调用方式 | 说明 |
|---|---|---|---|
| `on_register` | 软必须 | 异步 | 启动时调一次。缺失时记 warn，扩展仍启动 |
| `on_keepalive` | **硬必须** | 异步 | 每 keepalive_interval 调。缺失时扩展报错退出 |
| `on_discover` | **硬必须** | 同步 | 每 pull_interval 调，必须返回节点数组。缺失时扩展报错退出 |
| `on_deregister` | 软必须 | 异步 | pool 停止时调一次。缺失时记 warn |
| `on_watch` | 可选 | 长驻 | 有则启用推送，无则靠 discover 轮询兜底 |

### 2.2 内置默认：FileRegistry

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

---

## 三、健康检查器（HealthChecker）

```
interface HealthChecker {
  check(): array;   -- 返回 {status: Beacon::HEALTH_OK|HEALTH_DEGRADED|HEALTH_DEAD, metrics: {...}}
}
```

- **内置默认**：`FpmPoolHealthChecker`——master 视角自感知（RINIT/RSHUTDOWN 自计数 busy/idle + shm 汇总），报 FPM 池级健康
- **注入**：业务实现此接口，报业务级健康（"DB 连上了吗？缓存热了吗？依赖服务通吗？"）
- **keepalive 携带**：`keepalive($id, $health)` 把健康数据带去注册中心，注册中心/store 的健康是**自报**不是探活

### 3.1 链式注册 + 最差聚合

`registerHealthChecker()` 是**追加**不是覆盖——可注册多个 HealthChecker，链式执行，结果取最差状态。内置 `FpmPoolHealthChecker` 默认已注册（报进程级 busy/saturation），业务注入的是第二个（报依赖级 DB/缓存），两者组合。

聚合规则（最差优先）：

```
NOT_READY > DEAD > DEGRADED > OK

任一 NOT_READY → 整体 NOT_READY（启动中，不接流量）
任一 DEAD      → 整体 DEAD（摘除）
任一 DEGRADED  → 整体 DEGRADED（降优先级）
全部 OK        → 整体 OK（接全量流量）
```

### 3.2 三态对标业界

| beacon 三态 | gRPC Health | K8s probe | Consul | Envoy | 语义 |
|---|---|---|---|---|---|
| `HEALTH_OK` | SERVING | passing | passing | healthy | 可接全量流量 |
| `HEALTH_DEGRADED` | — | warning | warning | degraded（降优先级） | 饱和/降级，接少量流量 |
| `HEALTH_DEAD` | NOT_SERVING | critical | critical | unhealthy/ejected | 摘除，不接流量 |

---

## 四、持久化存储（Persistence）

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

---

## 五、钩子（Hooks）

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

### 5.1 取舍表

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

---

## 六、宣告地址解析器（AdvertiseResolver）

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

---

## 七、注入机制

### 7.1 注入 API（PHP userland）

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

### 7.2 注入时机

注入在 **治理 worker fork 后**完成（不是 MINIT——master 无 VM，不能执行 userland）。流程：

1. MINIT（master）：解析 INI → 初始化 shm → spawn 治理 worker（独立进程）
2. 治理 worker fork 后：初始化自己的 VM → 执行 `governance.php`（注入适配器对象）→ 进 timer 循环
3. FPM master fork FPM workers（注入对象不传递——FPM worker 不需要注入对象，只读 shm 取节点）

**关键约束**（PHP-FPM 进程模型决定）：
- master 无 VM，MINIT 不能执行 userland PHP——bootstrap 必须在治理 worker fork 后执行
- `auto_prepend_file` 是 per-request 的，每个请求都执行，**不适合**一次性注入
- 注入的 PHP 对象是 per-process 的，不能跨进程传递——但只有治理 worker 需要注入对象（注册/保活/发现），FPM worker 只读 shm，不碰注册中心

运行时不再改注入（避免热切换竞态）。

---

## 八、回调设计：必须、超时、异步化

### 8.1 三个核心原则

| 原则 | 说明 | 落地方式 |
|---|---|---|
| **回调是必须的** | 扩展不内置任何注册中心协议，没有回调 = 无法工作 | bootstrap 阶段校验；缺失必须回调 → 扩展拒绝启动 |
| **回调有硬超时** | 防止用户代码阻塞拖垮治理 worker | PHP 层自治超时（ReactPHP 协程，HTTP 客户端自带 timeout） |
| **回调建议异步化** | 注册/保活/注销是"写"操作，不需要等待结果 | PHP 层用 ReactPHP 协程；C 层 fire-and-forget |

### 8.2 异步化的具体含义

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

### 8.3 PHP 层异步化最佳实践

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
