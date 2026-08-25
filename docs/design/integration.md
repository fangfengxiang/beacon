# 集成与协同

> 本文档描述 php-beacon-extension 与 Yar RPC、beacon 网关的协同方式。

---

## 一、与 Yar RPC 协作

### 1.1 作为 Yar 服务端（provider）——零代码

PHP 服务用 `Yar_Server::handle($service_obj)` 暴露 RPC，**代码不变**。扩展在 FPM pool 启动时自动把"本服务 calc 在 10.0.0.5:8888"注册到注册中心。

```php
// calc.php — Yar 服务端，原样不变
$calc = new Calculator();
$server = new Yar_Server($calc);
$server->handle();
```

扩展读 INI 的 `service_name` + `advertise_host:port`，注册到注册中心。peer（其他 PHP 或 beacon）从注册中心发现 calc 服务在此地址。

**advertise 地址的必要性**：FPM 通常 listen `127.0.0.1:9000`，peer 连不到。前置 nginx 在 `10.0.0.5:8888` 转发到 FPM。注册的是 advertise 地址（nginx），不是 listen 地址（FPM）。对标 Consul 的 `advertise_addr`。

**注册内容**（写入注册中心的完整结构）：

```
/beacon/inst/{service}/{instance_id} = {
  host: "10.0.0.5",           ← advertise_host（前置 nginx 地址，非 FPM listen）
  port: 8888,                  ← advertise_port（前置 nginx 端口）
  status: "ok",                ← FPM pool 健康（自计数汇总，非单个 worker）
  methods: ["add", "sub"],     ← 服务方法列表（Beacon::serve 反射）
  registered_at: 1692800000,   ← 注册时间戳
  lease_id: 12345              ← etcd lease ID（保活用）
}
```

**实例 ID**：`{service_name}-{advertise_host}-{advertise_port}`，如 `calc-10.0.0.5-8888`。同一 host:port 只有一个 FPM pool 提供同一个服务，ID 唯一。对标 Consul 的 `{service}-{node}-{port}`。

**注册的是 FPM pool 的健康，不是单个 worker 的健康**——`FpmPoolHealthChecker` 读 shm 自计数（busy/idle/throughput），报整个 pool 的健康。外部连的是 advertise 地址（nginx），不是单个 worker。

### 1.2 作为 Yar 客户端（consumer）——client 侧 LB + failover

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

### 1.3 协议无关性

`Beacon::pick()` 返回的是节点（host/port/health），不创建 Yar client。PHP 代码自己创建 client——所以同一套发现机制可用于 gRPC-PHP、HTTP API、Thrift，不绑定 Yar：

```php
// gRPC-PHP 消费者同样用 Beacon::pick
$node = Beacon::pick('order');
$client = new Grpc\BaseStub("{$node['host']}:{$node['port']}");
```

### 1.4 可选高层封装（独立 package，非扩展）

扩展只提供 `pick()`（协议无关）。若要"一行调 Yar + 自动重试"，那是独立 package `beacon-yar` 的事，不塞进扩展（保持扩展协议无关）：

```php
// beacon-yar package（非扩展本体）
$result = BeaconYar::call('user', 'getProfile', [$uid]);  // pick + client + retry 封装
```

---

## 二、与 beacon 网关的协同

### 2.1 共享 etcd，职责分离

扩展和 beacon **用同一个 etcd**，但职责不重叠：

| 角色 | 职责 | 对 etcd |
|---|---|---|
| php-beacon-extension | PHP 自注册 + 保活（带自报健康）+ 本地节点缓存 | **写**（register/keepalive）+ 读（discover） |
| beacon 网关 | 路由/LB/故障转移/协议转化 | **读**（读 PHP 自注册的状态做路由决策） |

### 2.2 自注册 > 主动探活

扩展让 PHP 自报健康（master 视角 + 业务级），keepalive 携带权威健康数据。这**彻底消灭 beacon 主动探活的盲区**：
- 无 in-band 排队盲区（PHP 自己报，不用 beacon 探）
- 无网络分区误判（PHP 自己知道活着，不靠 beacon 猜）
- 无饱和检测延迟（PHP 实时报池状态，不等 3s 探活轮）

beacon 从"猜 PHP 健不健康"升级为"读 PHP 自报的健康"。

### 2.3 两种部署形态

**形态 A：扩展 + beacon 共存**（推荐）
- PHP 自注册到 etcd，beacon 从 etcd 读做路由
- 扩展消灭探活盲区，beacon 做网关层路由/LB/协议转化
- 两者协同，各取所长

**形态 B：仅扩展，无 beacon**
- PHP 自注册 + 自发现 + 本地缓存，PHP 直接调 peer（Yar client → peer Yar server）
- 纯 Go 模型，beacon 退化为只做协议转化（gRPC↔Yar）+ ingress
- 适合 PHP 间直连、不需要网关层路由的场景

### 2.4 数据流闭环

```
PHP(FPM+扩展) ──register/keepalive(带自报健康)──→ etcd
                                                      ↑
                          beacon 网关 ──read(路由决策)──┘
                              ↓
                     balancer 选健康 PHP 实例
                              ↓
                     请求 → PHP-FPM worker
```

PHP 自报健康（权威）→ etcd ← beacon 读（路由）。**自注册消灭探活盲区**：PHP 自己知道活着，不靠 beacon 猜。beacon 从"探活 PHP"升级为"读 PHP 自报"。
