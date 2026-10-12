## MODIFIED Requirements

### Requirement: 双缓冲无锁读写
shm SHALL 维护双 buffer（A/B），通过 header 的 active 字段原子切换。在单治理写者模型下，每批首次 store SHALL 从激活表复制快照到非激活表，后续 store 更新该批快照；commit SHALL 发布该快照，无 pending 更新的 commit SHALL 不切换。读者 SHALL 只查激活表，合法空表 SHALL 表示无节点，不能回退到含未提交数据的非激活表。并发读者保留协议需后续独立设计，本轮不承诺跨多次 commit 的读者保护。

#### Scenario: 治理 worker 写非激活 buffer 后切指针
- **GIVEN** 当前 active=0，服务 A/B 已提交
- **WHEN** 治理 worker 更新 A 后 commit
- **THEN** B 保留，更新前读取 A 为旧值，更新提交后读到新 A

#### Scenario: FPM worker 读激活 buffer 无锁
- **GIVEN** 治理 worker 正在写非激活 buffer，未 commit
- **WHEN** FPM worker 调用读接口
- **THEN** 读激活 buffer，不读取非激活 buffer

#### Scenario: 提交空表不回退
- **GIVEN** 激活 buffer 的目标 service 节点数为 0
- **WHEN** FPM worker 读该 service
- **THEN** 返回空表或 null，不复活旧节点

### Requirement: CRC32 校验与内存屏障
shm header SHALL 包含 checksum 字段。每次发布前 SHALL 对即将激活的完整节点表计算 CRC32，active 切换前后 SHALL 执行内存屏障。当前 checksum SHALL 作为提交诊断字段；读侧 SHALL 拒绝 writing 或超限 node_count，不得使用未提交表恢复。可靠的 CRC 读侧验证和已提交备份恢复需随并发快照协议另行实现，本轮不将该能力视为已实现。

#### Scenario: 提交计算 checksum
- **GIVEN** 单治理写者已 stage 节点更新
- **WHEN** commit 执行
- **THEN** 新表 CRC32 写入 checksum，并在发布前后执行内存屏障

#### Scenario: 明显损坏不读未提交表
- **GIVEN** 目标服务 writing 非零或 node_count 超过上限
- **WHEN** 读取该服务
- **THEN** 返回不可用，不尝试非激活表
