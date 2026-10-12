## Why

公开 API 缺少严格 PHPT 验收，双缓冲读回退会暴露未提交数据并复活已删除节点，输入转换和权重边界也会产生错误路由。本轮通过可复现测试驱动修复。

## What Changes

- **BREAKING** 删除非激活表读回退（旧实现会暴露未提交数据）；提交前隔离节点更新；增量提交保留其他服务；空节点表作为有效删除。
- 拒绝非法服务名，跳过无效节点，避免整数溢出与字段残留。
- 加权选择排除零权重；限制 setOpt 的策略枚举。
- 回调存储跟随请求生命周期，支持安全替换和自清除。
- 补充公开 API PHPT、测试说明与 review 记录。

## Capabilities

### New Capabilities
- `public-api-acceptance`: 公开 API 的快照、输入边界、选择及回调验收契约。

### Modified Capabilities

- `infra-shm`: 非激活表不能作为读回退；合法空表表示删除；checksum 保留为提交诊断字段，完整读侧损坏检测需另行设计。

## Impact

影响 beacon_shm.c、beacon_api_governance.c、beacon_service_select.c、beacon_api.c、beacon_callback.c、beacon.c、php_beacon.h 与 tests。无新增依赖。

对标 PHP 官方 PHPT 规范（https://wiki.php.net/doc/articles/writing-tests）与 Swoole 原生扩展测试（https://github.com/swoole/swoole-src）：使用独立场景、精确 EXPECT 和公开接口验证；沿用 Yar/Redis 的 setOpt 返回 bool 风格。服务节点零权重不参与 weighted 路由，与源码自身契约一致。
