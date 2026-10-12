## 1. Regression acceptance

- [x] 1.1 新增公开 API PHPT，构建原实现并确认回归失败。

## 2. Implementation

- [x] 2.1 beacon_shm.c：实现 staged 快照、提交隔离和空表语义。
- [x] 2.2 beacon_api_governance.c：节点转换和服务名边界校验。
- [x] 2.3 beacon_service_select.c：修复零权重选择。
- [x] 2.4 beacon_api.c：限制策略枚举并校验读侧服务名。
- [x] 2.5 beacon_callback.c：请求局部存储、安全替换和调用引用。
- [x] 2.6 beacon.c：请求初始化及销毁回调。

## 3. Validation and documentation

- [x] 3.1 完整 PHPT 验收、更新 CI 和 package.xml 测试清单。
- [x] 3.2 编写 review 发现、学习路线、验收说明并校验 OpenSpec。
