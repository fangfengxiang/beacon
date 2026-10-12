# PHPT 验收

构建并运行（PHP 8.x、phpize/php-config、SysV IPC）：

```sh
phpize
./configure --enable-beacon
make -j4
NO_INTERACTION=1 REPORT_EXIT_STATUS=1 make test
NO_INTERACTION=1 REPORT_EXIT_STATUS=1 make test TESTS='tests/public_*.phpt'
```

`REPORT_EXIT_STATUS=1` 让失败产生非零退出码。本轮在 macOS arm64 / PHP 8.5.11 NTS 验收：22 PASS，0 SKIP，0 FAIL。Linux PHP 8.0–8.5 由 CI 执行，本地未运行其他版本。

| 用例 | 验收内容 |
|---|---|
| public_snapshot | 提交前不可见、增量保留其他服务、空表删除、空提交不复活 |
| public_node_validation | 必填字段、NUL/长度、端口、状态、权重，跳过节点不污染后续节点 |
| public_service_names | 63 字节合法；空、超长和 NUL 不产生别名 |
| public_weighted | 零权重排除，全零返回 null；不依赖抽样比例 |
| public_selection | 健康过滤、degraded 回退、exclude、三种策略、可选 methods |
| public_options | 策略枚举、正数选项、未知 key、无效回调 |
| public_disabled | 关闭时的结构、返回值和警告数量 |
| public_metadata | 公开静态方法、必填参数、Zend TypeError/ArgumentCountError |
| public_health | readiness、metrics、饱和度健康状态 |
| public_capacity | 64 节点限制、固定字符串长度、缩容 |
| public_callback | 替换/清除引用、异常隔离、自清除 |
| public_request_lifetime | 同一 PHP 服务进程连续三次请求，回调不能跨请求保留 |

共享内存测试显式使用独立十进制 key，并设置 `BEACON_GOVERNANCE_WORKER=1`，防止测试启动真实治理 worker；该设置在被测扩展 MINIT 前生效。正常退出由扩展删除创建的段。固定 key 的同一用例不应跨多个 runner 同时运行，需隔离容器或修改 key。IPC 不可用会失败，不会静默 skip；权限受限的沙箱需要允许本机 IPC 的测试环境。

多请求测试通过 `proc_open` 启动临时 PHP 内置服务，动态端口，三次 HTTP 请求后 finally 终止进程；需要本机 TCP。路由 fixture 为 `.inc`，不会被 runner 误当独立测试。回调执行和健康计数使用现有 `beacon_test_*` hook，其余走公开 API。

004 改为精确结构预期；005 验收校准后的 busy 计数，009 隔离预期日志；010 精确验收治理 worker guard。010 不再宣称验收完整 spawn/restart/shutdown。FPM fork/exec、reload、多写者、并发损坏恢复及 ZTS 不在本轮验收范围。

参考：[PHP PHPT 编写规范](https://wiki.php.net/doc/articles/writing-tests)、[Swoole 原生测试流程](https://github.com/swoole/swoole-src)。
