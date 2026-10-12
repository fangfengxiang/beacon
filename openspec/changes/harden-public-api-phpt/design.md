## Context

按 docs/design/api-reference.md、shm-design.md 的单治理写者模型迭代。当前读备份不校验 CRC，也不区分合法空表和损坏；回调 zval 在 MSHUTDOWN 才释放，超过请求内存生命周期。

## Goals / Non-Goals

Goals: 公开 API 精确验收、已提交快照可见、输入范围安全、请求内回调引用安全。
Non-Goals: 本轮不宣称解决共享内存并发读写协议、FPM master/reload 所有权、治理 worker 完整注册中心集成。

## Decisions

- 首次 store 复制当前快照到非激活表，以模块全局 staged 标记保持同批更新；commit 无 pending 时不切换。读者只读 active，空表有效，不回退到未提交表。
- 服务名必须 1..63 字节且无 NUL；节点必须有合法 id/host/port，status 支持既有字符串和整数范围，weight 为 0..65535。无效节点告警并跳过，每条转换前清空目标，避免部分写入污染下一条。
- weighted 的零权重贡献为零，全零返回 null；setOpt 限制枚举 1..3。
- 回调 zval 存储在模块全局，RINIT 初始化，RSHUTDOWN 清理；调用前持有临时引用，允许回调自己清除或替换注册槽。
- 新 PHPT 用公开方法验收，仅回调执行用既有内部 hook；精确 EXPECT，不做概率分布断言。每个用例使用独立十进制 IPC key，失败自动清理通过扩展退出路径。

## Risks / Trade-offs

- 双缓冲仍缺少读者保留协议 → 明确记录为后续高优先级工作；不宣称无锁并发安全已验收。
-  stricter 输入会跳过过去被截断或溢出的节点 → 文档列明契约。
- 仅本机 PHP 8.5 NTS 可实测 → CI 扩展 PHP 8.5，Linux/FPM/ZTS 另列验证边界。
