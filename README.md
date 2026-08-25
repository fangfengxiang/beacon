# php-beacon-extension

> PHP-FPM 状态信标与治理调度基底扩展

## REQUIREMENT

- PHP >= 8.0
- C11 编译器（gcc >= 4.9 / clang >= 3.6）
- sysv shm

## INTRODUCTION

php-beacon-extension 是 PHP-FPM 的 C 扩展，解决 FPM 进程短生命周期与服务治理需长连接/状态持续性的矛盾。

扩展只做 FPM 做不到的三件事：**感知**（自计数 pool 级健康）、**调度**（spawn 常驻治理 worker）、**缓存**（双缓冲 shm，FPM worker 零 I/O 读取 peer 节点）。注册中心通信、健康检查逻辑、服务发现协议，全部交给 PHP 层注入实现。扩展不绑定任何注册中心，不绑定任何协议。

**beacon 的意象**：beacon = 信标 = 主动发光的信号灯。传统探活是"外部用手电筒照 PHP"（发 HTTP 探针猜 PHP 状态），这个扩展是"PHP 自己亮灯"（自计数 → 自报健康 → 通过回调发出去）。

## WHEN TO USE

**适用**：
- PHP-FPM 微服务需要自注册/自发现/自报健康
- 需要 client 侧 LB + failover（不经网关）
- 需要 pool 级健康感知（busy/idle/saturation）

**不适用**：
- 需要内置注册中心客户端（扩展不绑定注册中心，由 PHP 回调注入）
- 需要网关层路由/协议转化（那是 [lua-resty-php-beacon](https://github.com/yar-group/lua-resty-php-beacon) 的事）
- 需要熔断/限流（留给上层组件）

## FEATURES

- 自计数 pool 级健康（RINIT/RSHUTDOWN 原子操作，10k req/s 下开销 ≈ 0.4ms/s）
- spawn 常驻治理 worker（fork + exec，带 VM，内置 FileRegistry 零配置）
- 双缓冲 shm（C 结构体 packed binary，FPM worker 零反序列化直读，`pick()` 延迟 < 1μs）
- 内置 LB（round-robin / random / weighted）
- client 侧 failover（`OPT_EXCLUDE` 排除已试节点）
- 四态健康模型（NOT_READY / OK / DEGRADED / DEAD）
- 零配置模式（两行 INI 即可运行）

## INSTALL

```bash
phpize
./configure --enable-beacon
make
sudo make install
```

`make install` 安装：
- `beacon.so` → PHP 扩展目录
- `governance.php` → 内置治理脚本（FileRegistry + keepalive/discover 循环）

## RUNTIME CONFIGURATION

| 指令 | 默认 | 说明 |
|------|------|------|
| `beacon.enabled` | 0 | 开关 |
| `beacon.service_name` | "" | 本服务名（provider 身份） |
| `beacon.advertise_host` | "" | 对外地址（peer 连此，非 FPM listen）。空 = 自动探测 |
| `beacon.advertise_host_env` | "" | 从环境变量读 advertise_host（K8s downward API） |
| `beacon.advertise_port` | "" | 对外端口。空 = 自动探测 |
| `beacon.registry_endpoint` | "" | 注册中心地址。空 = 文件模式（`/var/run/beacon/`） |
| `beacon.governance_bin` | "" | PHP CLI 路径。空 = 编译宏 |
| `beacon.governance_script` | "" | 治理脚本路径。空 = 内置默认 |
| `beacon.keepalive_interval` | 3 | 保活间隔（秒） |
| `beacon.pull_interval` | 2 | 拉取间隔（秒） |
| `beacon.heartbeat_ttl` | 15 | 注册中心 lease/TTL（秒） |
| `beacon.health_dead_threshold` | 3 | 连续 dead 次数才报 dead |
| `beacon.lb_strategy` | "round_robin" | 默认 LB 策略 |
| `beacon.shm_key` | 0 | IPC shm key。0 = 自动 |
| `beacon.log_file` | "" | 日志文件路径。空 = stderr |
| `beacon.log_level` | "warn" | 日志级别（debug/info/warn/error） |

**零配置模式**：

```ini
extension=beacon.so
beacon.enabled = 1
beacon.service_name = "calc"
```

## CONSTANTS

### 选项 key（`Beacon::OPT_*`，用于 `setOpt()` 的 key）

| 常量 | 值 | 类型 | 说明 |
|------|-----|------|------|
| `Beacon::OPT_ON_REGISTER` | 1 | callable | 服务注册回调（软必须） |
| `Beacon::OPT_ON_KEEPALIVE` | 2 | callable | 保活回调（**硬必须**） |
| `Beacon::OPT_ON_DISCOVER` | 3 | callable | 节点发现回调（**硬必须**） |
| `Beacon::OPT_ON_DEREGISTER` | 4 | callable | 服务注销回调（软必须） |
| `Beacon::OPT_ON_WATCH` | 5 | callable | 节点监听回调（可选） |
| `Beacon::OPT_LB_STRATEGY` | 6 | int | LB 策略 |
| `Beacon::OPT_KEEPALIVE_INTERVAL` | 7 | int | 保活间隔（秒） |
| `Beacon::OPT_PULL_INTERVAL` | 8 | int | 拉取间隔（秒） |
| `Beacon::OPT_HEARTBEAT_TTL` | 9 | int | 注册中心 lease/TTL（秒） |
| `Beacon::OPT_HEALTH_DEAD_THRESHOLD` | 10 | int | 连续 dead 次数阈值 |
| `Beacon::OPT_EXCLUDE` | 11 | array | `pick()` 专用：已试节点 id 数组 |
| `Beacon::OPT_PREFER_HEALTHY` | 12 | bool | `pick()` 专用：是否只选健康节点 |

> **Note**: `OPT_EXCLUDE` 和 `OPT_PREFER_HEALTHY` 是 `pick()` 的 opts 数组 key，不支持 `setOpt()`。

### LB 策略（`Beacon::LB_*`）

| 常量 | 值 | 说明 |
|------|-----|------|
| `Beacon::LB_ROUND_ROBIN` | 1 | 轮询（默认） |
| `Beacon::LB_RANDOM` | 2 | 随机 |
| `Beacon::LB_WEIGHTED` | 3 | 加权 |

### 健康状态（`Beacon::HEALTH_*`）

| 常量 | 值 | 说明 |
|------|-----|------|
| `Beacon::HEALTH_NOT_READY` | "not_ready" | 启动中/预热未完成，不接流量 |
| `Beacon::HEALTH_OK` | "ok" | 健康，接全量流量 |
| `Beacon::HEALTH_DEGRADED` | "degraded" | 饱和/降级，接少量流量（降优先级） |
| `Beacon::HEALTH_DEAD` | "dead" | 死亡，摘除不接流量 |

## BEACON API

> **Note**: `Beacon` 是 final 类，所有方法为静态方法，不可实例化。

### pick

```php
Beacon::pick(string $service, array $opts = []): ?array
```

内置 LB 选一个实例。返回 `{id, host, port, status, weight, methods}` 或 null（无可用节点）。

```php
<?php
$node = Beacon::pick('user');
if ($node === null) {
    throw new RuntimeException('user 服务无可用节点');
}

// 协议无关：pick() 返回节点信息，client 由你自己创建
$client = new Yar_Client("http://{$node['host']}:{$node['port']}/user");
$result = $client->getProfile($uid);
```

**failover**：

```php
<?php
$tried = [];
while ($node = Beacon::pick('user', [Beacon::OPT_EXCLUDE => $tried])) {
    $tried[] = $node['id'];
    $client = new Yar_Client("http://{$node['host']}:{$node['port']}/user");
    try {
        $result = $client->getProfile($uid);
        break;
    } catch (Yar_Server_Exception $e) {
        continue;
    }
}
```

### getInstances

```php
Beacon::getInstances(string $service): array
```

取该服务全部健康实例（OK + DEGRADED），排除 DEAD/NOT_READY。

```php
<?php
$nodes = Beacon::getInstances('user');
foreach ($nodes as $node) {
    echo "{$node['host']}:{$node['port']} ({$node['status']})\n";
}
```

### ready

```php
Beacon::ready(): bool
```

标记本实例预热完成，健康从 NOT_READY 转 OK。

```php
<?php
// 业务预热完成后调用
Beacon::ready();
```

### status

```php
Beacon::status(): array
```

返回当前 pool 状态，供 health check endpoint 调用。

```php
<?php
$status = Beacon::status();
// [
//   'enabled' => true,
//   'mode' => 'normal',           // normal | degraded
//   'health' => 'ok',             // not_ready | ok | degraded | dead
//   'governance_alive' => true,
//   'governance_pid' => 12345,
//   'cache_age_seconds' => 5,
//   'pool_ready' => true,
//   'pool_busy' => 8,
//   'pool_idle' => 2,
//   'pool_total' => 10000,
//   'saturation' => 0.8,
// ]
```

### setOpt

```php
Beacon::setOpt(int $key, mixed $value): bool
```

设置运行时选项。对标 `Yar_Client::setOpt()` / `curl_setopt()`。

```php
<?php
// 设置 LB 策略
Beacon::setOpt(Beacon::OPT_LB_STRATEGY, Beacon::LB_RANDOM);

// 设置回调（治理 worker 进程内调用）
Beacon::setOpt(Beacon::OPT_ON_KEEPALIVE, function(array $ctx) {
    // $ctx = ['service' => 'calc', 'instance' => [...], 'health' => [...]]
});
```

### reportHealth

```php
Beacon::reportHealth(array $health): bool
```

业务主动报健康。`$health` 数组至少包含 `'status'` 键（`Beacon::HEALTH_*` 字符串常量）。

```php
<?php
Beacon::reportHealth(['status' => Beacon::HEALTH_DEGRADED]);
```

### deregister

```php
Beacon::deregister(): bool
```

手动注销。pool stop 时治理 worker 自动 deregister，一般无需手动调。

## GOVERNANCE API

> **Note**: `Beacon\Governance` 仅在 CLI SAPI（治理 worker 进程）中可用。

### storeNodes

```php
Beacon\Governance::storeNodes(string $service, array $nodes): bool
```

把 PHP 数组转为 C 结构体写入 shm（非激活 buffer）。

```php
<?php
Beacon\Governance::storeNodes('calc', [
    ['id' => 'calc-1', 'host' => '10.0.0.1', 'port' => 9000, 'status' => 'ok', 'weight' => 1],
    ['id' => 'calc-2', 'host' => '10.0.0.2', 'port' => 9000, 'status' => 'ok', 'weight' => 2],
]);
```

### commit

```php
Beacon\Governance::commit(): bool
```

原子切 active buffer，FPM worker 立即可见新数据。`storeNodes` 后调用。

```php
<?php
Beacon\Governance::storeNodes('calc', $nodes);
Beacon\Governance::commit();
```

### calcHealth

```php
Beacon\Governance::calcHealth(): array
```

读 shm 自计数，算 pool 级健康状态。治理 worker keepalive 前调用。

```php
<?php
$health = Beacon\Governance::calcHealth();
// ['status' => 'ok', 'pool_busy' => 2, 'pool_idle' => 8,
//  'pool_total' => 100, 'saturation' => 0.2, 'governance_alive' => true]
```

## ARCHITECTURE

```
FPM master（长生命周期）
  ├─ MINIT: 初始化 shm → spawn 治理 worker（fork + close_fds + prctl + exec）
  ├─ 监控: kill(governance_pid, 0) 检测，崩溃自动重启（重试上限 5 次）
  │
  ├─ governance worker（独立进程，带 VM，常驻）
  │   ├─ 简单循环（usleep + time check，零 composer 依赖）
  │   ├─ keepalive timer(3s) → calcHealth() + 调注入的 on_keepalive 回调
  │   ├─ discover timer(2s)  → 调注入的 on_discover 回调 → 写 shm
  │   └─ 崩溃: prctl 确保旧 master 退出时自动 deregister
  │
  └─ FPM worker 1..N（请求级）
      ├─ RINIT:    shm atomic inc(busy) + 心跳槽位写 pid+time
      ├─ Beacon::pick(): 读 shm C 结构体，零反序列化，< 1μs
      └─ RSHUTDOWN: shm atomic dec(busy) + 心跳槽位 busy=0
```

**关键隔离**：治理 worker 和 FPM worker 是独立进程——worker 崩不影响治理（治理继续保活），治理崩不影响 worker（worker 仍服务请求，节点缓存陈旧直到重启）。

**请求路径零 I/O**：FPM worker 请求路径只碰 shm（原子操作 + 读），所有注册中心 I/O 在治理 worker。

## INTEGRATION WITH YAR

**作为 Yar 服务端（provider）——零代码**：

```php
<?php
// calc.php — Yar 服务端，原样不变
$calc = new Calculator();
$server = new Yar_Server($calc);
$server->handle();
```

扩展在 FPM pool 启动时自动把"本服务 calc 在 10.0.0.5:8888"注册到注册中心。

**作为 Yar 客户端（consumer）——client 侧 LB + failover**：

```php
<?php
$node = Beacon::pick('user');
$client = new Yar_Client("http://{$node['host']}:{$node['port']}/user");
$result = $client->getProfile($uid);
```

**协议无关**：`pick()` 返回的是节点（host/port/health），不创建 client。同一套发现机制可用于 gRPC-PHP、HTTP API、Thrift，不绑定 Yar。

## ERROR HANDLING

遵循 PHP 扩展惯例：用返回值表示状态，不用异常表示状态。

- `pick()` 返回 `?array`，null = 无可用节点
- `getInstances()` 返回 `array`，空数组 = 无节点
- `setOpt()` 返回 `bool`，false = 无效 key 或 value
- 内部错误（shm 不可用）→ `php_error_docref` 警告 + 返回 false/null

## DESIGN DOCUMENTS

设计文档位于 `docs/design/`（13 个文件），索引见 `docs/design/README.md`。架构决策记录（ADR）位于 `docs/adr/`，共 35 个设计决策。

## LICENSE

MIT
