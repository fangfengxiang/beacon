# php-beacon-extension 设计文档

> 状态：设计稿（经 Kimi 技术评审修正）
> 基础：`php-fpm-health-extension.md`（早期健康感知探讨）
> 关联：`lua-resty-php-beacon-plan.md`（beacon 网关）、`lua-resty-php-beacon-etcd-design.md`（etcd 操作）
> 评估：`evaluation.md`（Kimi 技术评审报告评估与取舍更新）
> 核心命题：把 Swoole 式的自注册基底通过扩展硬塞进 FPM，让 PHP 成为注册中心一等公民

---

## 定位

**php-beacon-extension 是 PHP-FPM 的"状态信标与治理调度基底"。**

它解决一个核心矛盾：**FPM 进程是短生命周期的，但服务治理需要长连接和状态持续性感知。**

扩展只做 FPM 做不到的三件事：

1. **感知** —— 自计数 pool 级健康状态（busy/idle/throughput）
2. **调度** —— spawn 常驻治理 worker，维持 timer 循环，驱动 PHP 回调
3. **缓存** —— 双缓冲 shm（C 结构体），让 FPM worker 零 I/O 读取 peer 节点

注册中心通信、健康检查逻辑、服务发现协议，全部交给 PHP 层注入实现。扩展不绑定任何注册中心，不绑定任何协议。

**beacon 的意象**：beacon = 信标 = 主动发光的信号灯。传统探活是"外部用手电筒照 PHP"（beacon 网关发 HTTP 探针猜 PHP 状态），这个扩展是"PHP 自己亮灯"（自计数 → 自报健康 → 通过回调发出去）。

---

## 设计原则

### 1. 扩展 = 状态感知 + 调度基底 + 回调注入

- **状态感知**：RINIT/RSHUTDOWN 自计数 pool 级健康状态（per-worker 心跳槽位，64B 对齐）
- **调度基底**：spawn 常驻治理 worker（fork + close_all_fds + prctl + exec），ReactPHP 协程调度 IO
- **回调注入**：注册中心通信、健康检查逻辑、服务发现协议，全部交给 PHP 层注入实现

### 2. 每个可变点允许业务层注入 PHP 回调

| 可变点 | 扩展提供 | 业务注入 |
|---|---|---|
| 注册中心通信 | C 层回调调度（`zend_call_function`，无 C 层超时） | PHP 回调实现 register/keepalive/discover/deregister/watch |
| 健康检查算法 | C 层自计数（RINIT/RSHUTDOWN 原子操作 + 心跳槽位校准） | PHP 回调返回业务级健康状态 |
| 节点信息存储 | C 层 shm 双缓冲（C 结构体，无锁读写） | PHP 回调返回节点列表，C 层写 shm |
| 钩子（副作用处理） | C 层回调调度 | PHP 回调实现 on_health_change/on_keepalive_fail 等 |
| 宣告地址解析 | INI 配置 + 自动探测 | PHP 函数返回 {host, port}（云 metadata/多网卡/K8s） |

### 3. 不绑定注册中心、不绑定协议、不绑定存储

- 注册中心：本地文件（默认）/etcd/Consul/Nacos，由注入决定
- 协议：服务任意 PHP 服务（Yar/gRPC-PHP/HTTP API/Thrift），不绑定 Yar
- 存储：shm（L1）/文件（L2 默认）/Redis/MySQL，由注入决定

### 4. 配置化是地基

- INI 是唯一配置源，8 个核心项
- 零配置模式：只配 `beacon.enabled = 1` + `beacon.service_name = "calc"` 就能跑
- `bootstrap.php` 只用于代码逻辑注入，不用于配置

### 5. 极简

- C 扩展 ~8 个文件，核心代码量 < 2000 行
- 无内置重试、无异常类、无 C 层超时
- 请求路径零 I/O（shm 直读，纳秒级）

---

## 文档导航

| 文档 | 内容 | 读者 |
|------|------|------|
| [beacon-extension-summary.md](beacon-extension-summary.md) | 设计总结：定位、原则、架构、模块全景 | 全体 |
| [evaluation.md](evaluation.md) | Kimi 技术评审报告评估与取舍更新 | 设计者 |
| [architecture.md](architecture.md) | 架构：治理 worker、进程模型、IPC、运行原理与数据流 | 架构师 |
| [shm-design.md](shm-design.md) | 共享内存设计：C 结构体、双缓冲、心跳槽位 | C 开发者 |
| [governance-worker.md](governance-worker.md) | 治理 worker 生命周期、spawn、状态机 | C 开发者 |
| [spi.md](spi.md) | SPI 回调注入设计：注册中心、健康检查、持久化、钩子 | PHP 开发者 |
| [api-reference.md](api-reference.md) | API 与配置参考：PHP userland API、INI 指令、常量 | PHP 开发者 |
| [usage-guide.md](usage-guide.md) | PHP 使用指南：安装、配置、三层使用模型 | 业务开发者 |
| [integration.md](integration.md) | 与 Yar RPC 协作、与 beacon 网关协同 | 架构师 |
| [acceptance.md](acceptance.md) | 验收标准、取舍与边界 | QA / SRE |
| [industry-benchmark.md](industry-benchmark.md) | 业界对标与设计溯源、概念词汇表 | 设计者 |
| [mvp.md](mvp.md) | 最小实现 MVP 路径、实现要求 | 项目经理 |

---

## 核心架构（一图流）

```
FPM master
  ├─ MINIT: 初始化 shm（C 结构体）→ fork + close_all_fds + prctl + exec PHP 治理脚本
  └─ 监控: kill(governance_pid, 0) 检测，崩溃重启（最多 5 次后停止）

治理 worker（php /usr/share/php/beacon/governance.php，独立 CLI 进程）
  ├─ ReactPHP event loop（协程调度 IO，不阻塞 timer）
  ├─ 内置 FileRegistry（默认）：写 /var/run/beacon/{service}.json + C API storeNodes/commit
  ├─ 可选：自定义脚本接入 etcd/Consul（PHP 层自治超时，C 层不干预）
  └─ 崩溃：prctl 确保旧 master 退出时自动 deregister

FPM worker（请求级）
  ├─ RINIT: per-worker 心跳槽位（64B 对齐）busy=1 + pool_busy 原子自计数
  ├─ Beacon::pick(): 读 shm C 结构体，零反序列化，< 1μs
  └─ RSHUTDOWN: busy=0，pool_busy 原子减，校准机制兜底泄漏
```

---

## 与原设计文档的关系

本文档集是 `php-beacon-extension-design.md`（2430 行）的拆分和更新版本。主要变化：

1. **架构修正**：基于 Kimi 技术评审报告的逐章 battle 结果
2. **文档拆分**：按功能域拆分为 12 个独立文档，每个聚焦一个主题
3. **内容更新**：反映 C 结构体、ReactPHP、文件模式默认注册中心等关键决策
4. **精简配置**：INI 从 13 个砍到 8 个，砍掉 configure() 数组

原设计文档仍保留在 `docs/php-beacon-extension-design.md` 作为历史参考。
