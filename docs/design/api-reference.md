# API 与配置参考

> 本文档描述 php-beacon-extension 的 PHP userland API、INI 指令、预定义常量。

---

## 一、PHP userland API

### 1.1 核心 API

| API | 签名 | 用途 | 调用时机 |
|---|---|---|---|
| `Beacon::pick(string $service, array $opts = [])` | `?array` | 内置 LB 选一个实例。返回 `{id, host, port, status, methods, ...}` 或 null（无可用实例） | 请求内按需 |
| `Beacon::getInstances(string $service)` | `array` | 取该服务全部健康实例。返回 `[{id, host, port, status, methods, ...}, ...]` | 请求内按需 |
| `Beacon::ready()` | `bool` | 标记本实例预热完成，健康从 NOT_READY 转 OK。实现：写 shm `pool.ready = 1`，治理 worker 下次 keepalive 时读到并转 OK | 预热完成后调用一次 |
| `Beacon::reportHealth(array $health)` | `bool` | 业务主动报健康。实现：写 shm `pool.business_health = $health`，治理 worker 的 HealthChecker 链式执行时合并业务报的健康（与 FPM 池感知取最差） | 请求内按需 |
| `Beacon::deregister()` | `bool` | 手动注销。实现：写 shm `pool.deregister = 1`，治理 worker 下次 tick 时读到并执行 `registry.deregister()`。pool stop 时治理 worker 自动 deregister，无需手动调 | 极少 |
| `Beacon::status()` | `array` | 返回当前 pool 状态（governance_pid、governance_status、cache_age_seconds、mode 等），供 health check endpoint 调用 | 请求内按需 |

### 1.2 治理 worker API（CLI SAPI 专用）

| API | 签名 | 用途 | 调用时机 |
|---|---|---|---|
| `Beacon\Governance::storeNodes(string $service, array $nodes)` | `bool` | 把 PHP 数组转为 C 结构体写入 shm（非激活 buffer） | 治理 worker discover 后 |
| `Beacon\Governance::commit()` | `bool` | 原子切 active buffer，FPM worker 立即可见新数据 | storeNodes 后 |
| `Beacon\Governance::calcHealth()` | `array` | 读 shm 自计数，算 pool 级健康状态 | 治理 worker keepalive 前 |

### 1.3 pick() 的 opts

- `Beacon::OPT_EXCLUDE`: 已试节点 id 数组（failover 用）
- `Beacon::OPT_LB_STRATEGY`: 临时覆盖 LB 策略（`Beacon::LB_ROUND_ROBIN` / `Beacon::LB_RANDOM` / `Beacon::LB_WEIGHTED`）
- `Beacon::OPT_PREFER_HEALTHY`: bool，是否只选 HEALTHY（默认 true，DEGRADED 排后）

---

## 二、INI 指令（php.ini / FPM pool conf）

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

---

## 三、预定义常量

对标 PHP 扩展惯例（`YAR_OPT_*` / `CURLOPT_*` / `Redis::OPT_*`），选项 key 用整数常量，枚举值用常量消除魔术字符串。

### 3.1 选项 key 常量（`Beacon::OPT_*`，整数，用于 `setOpt()` 的 key）

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

### 3.2 LB 策略枚举（`Beacon::LB_*`，用于 `OPT_LB_STRATEGY` 的值）

| 常量 | 值 | 说明 |
|---|---|---|
| `Beacon::LB_ROUND_ROBIN` | 1 | 轮询（默认） |
| `Beacon::LB_RANDOM` | 2 | 随机 |
| `Beacon::LB_WEIGHTED` | 3 | 加权（按节点健康/负载分配） |

### 3.3 健康状态枚举（`Beacon::HEALTH_*`，用于 `HealthChecker::check()` 返回的 status）

| 常量 | 值 | 说明 |
|---|---|---|
| `Beacon::HEALTH_NOT_READY` | "not_ready" | 启动中/预热未完成，不接流量 |
| `Beacon::HEALTH_OK` | "ok" | 健康，接全量流量 |
| `Beacon::HEALTH_DEGRADED` | "degraded" | 饱和/降级，接少量流量（降优先级） |
| `Beacon::HEALTH_DEAD` | "dead" | 死亡，摘除不接流量 |

常量在扩展 MINIT 阶段用 `REGISTER_LONG_CONSTANT` / `REGISTER_STRING_CONSTANT` 注册，PHP userland 通过 `Beacon::OPT_*` / `Beacon::LB_*` 等访问。对标 `YAR_OPT_PACKAGER` 的注册方式。

---

## 四、错误处理

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
