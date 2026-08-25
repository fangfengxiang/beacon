# PHP 使用指南

> 本文档描述 php-beacon-extension 的安装、配置、使用方式。

---

## 一、安装

### 1.1 源码编译（推荐）

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

### 1.2 GitHub Release 预编译（二期）

```bash
curl -fsSL https://beacon.sh/install.sh | bash
```

### 1.3 PECL（三期，不主推）

```bash
pecl install beacon
```

---

## 二、配置

### 2.1 最小配置（零配置模式）

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

### 2.2 完整配置

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

---

## 三、三层使用模型

| 层 | 谁做 | 何时 | PHP 代码 |
|---|---|---|---|
| **自注册** | 扩展自动 | FPM pool 启动 | 无（零代码，扩展 fork worker 自动注册） |
| **保活/发现** | 治理 worker 自动 | 后台循环 | 无（worker 跑 timer，PHP 不感知） |
| **取节点** | PHP 业务代码按需 | 请求内需要调 peer 时 | `Beacon::pick($service)` 一行 |

**关键**：PHP 业务代码只在**取节点**这一步显式调 API。自注册和保活是扩展后台自动做的，PHP 代码完全不碰。

---

## 四、不注入任何实现（文件模式，零依赖）

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

---

## 五、注入实现（需要定制时）

### 5.1 自定义注册中心（如 Consul）

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

### 5.2 自定义治理脚本

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

---

## 六、与 Yar RPC 协作

### 6.1 作为 Yar 服务端（provider）——零代码

PHP 服务用 `Yar_Server::handle($service_obj)` 暴露 RPC，**代码不变**。扩展在 FPM pool 启动时自动把"本服务 calc 在 10.0.0.5:8888"注册到注册中心。

```php
// calc.php — Yar 服务端，原样不变
$calc = new Calculator();
$server = new Yar_Server($calc);
$server->handle();
```

扩展读 INI 的 `service_name` + `advertise_host:port`，注册到注册中心。peer（其他 PHP 或 beacon）从注册中心发现 calc 服务在此地址。

### 6.2 作为 Yar 客户端（consumer）——client 侧 LB + failover

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

### 6.3 协议无关性

`Beacon::pick()` 返回的是节点（host/port/health），不创建 Yar client。PHP 代码自己创建 client——所以同一套发现机制可用于 gRPC-PHP、HTTP API、Thrift，不绑定 Yar：

```php
// gRPC-PHP 消费者同样用 Beacon::pick
$node = Beacon::pick('order');
$client = new Grpc\BaseStub("{$node['host']}:{$node['port']}");
```

---

## 七、服务声明机制

### 7.1 三层服务声明

```
层 1（INI 静态）    beacon.service_name = "calc"           → 治理 worker 启动即注册
层 2（bootstrap）   Beacon::serveAll(['calc'])   → 治理 worker 启动时注册（PHP 写）
层 3（入口文件）    Beacon::serve('calc', $obj)             → 请求来时声明，治理 worker 下次 tick 注册
```

- **层 1/2 解决冷启动**：pool 刚启动还没请求来，治理 worker 就能注册（避免"没人来就不注册，不注册就没人来"的死锁）
- **层 3 解决方法列表自动反射 + handler 附近声明**（符合"声明与 handler 同源"的期望）

### 7.2 `Beacon::serve()` API

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

### 7.3 推荐用法

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

---

## 八、健康检查

### 8.1 内置 FPM 池感知

扩展内置 `FpmPoolHealthChecker`——master 视角自感知（RINIT/RSHUTDOWN 自计数 busy/idle + shm 汇总），报 FPM 池级健康。

### 8.2 业务级健康检查

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

### 8.3 预热完成标记

```php
// 业务预热完成后调用
Beacon::ready();  // 健康从 NOT_READY 转 OK
```

---

## 九、故障排查

### 9.1 查看治理 worker 状态

```bash
# 治理 worker 是否存活
ps aux | grep beacon-governance

# 查看治理 worker 日志
tail -f /var/log/beacon/governance.log | jq .

# 查看 FPM 扩展错误
tail -f /var/log/php-fpm/www-error.log | grep beacon
```

### 9.2 查看注册状态

```bash
# 文件模式
cat /var/run/beacon/calc.json

# etcd 模式
etcdctl get /beacon/inst/calc/ --prefix
```

### 9.3 查看 pool 健康状态

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
