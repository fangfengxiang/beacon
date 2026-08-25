# Yaconf 跨 Worker 配置共享 — 学习笔记

> 来源：https://github.com/laruence/yaconf
> 作者：Xinchen Hui (laruence@php.net)
> 版本：1.2.1-dev
> 学习日期：2026-08-26

---

## 一、解决什么问题

PHP 应用每次请求都解析 `.ini`/`.php` 配置文件，付出 I/O 和解析成本后即丢弃。Yaconf 在 PHP 启动时解析一次，永久从内存服务。

**核心矛盾**：配置是只读的，但每个 FPM worker 是独立进程——如何让所有 worker 共享同一份配置，而不复制 N 份？

## 二、核心方案：不可变数据 + fork COW

**不用共享内存（shmget/mmap），用操作系统内核的写时复制（Copy-on-Write）。**

```
FPM master
  │
  │  MINIT（fork workers 之前）
  │  ├─ 解析 INI 文件 → 临时内存（emalloc）
  │  ├─ 两阶段压缩 → 单个连续内存块（pemalloc，persistent）
  │  │   ├─ Phase 1: 遍历解析树，收集所有字符串和 HashTable
  │  │   ├─ Phase 2: 分配一整块内存，按内容去重字符串，
  │  │   │           重新布局 HashTable 为引擎规范布局
  │  │   └─ 释放解析阶段的分散分配
  │  └─ 配置树标记为 IS_ARRAY_IMMUTABLE + interned strings
  │
  │  fork() ──────────────────────────────────────────────
  │
  ├─ worker 1 ─┐
  ├─ worker 2 ─┤  OS 内核 COW：所有 worker 共享同一物理内存页
  ├─ worker 3 ─┤  零额外内存，零系统调用，零代码
  └─ worker N ─┘
```

**为什么 COW 适合配置共享**：
- 配置是**只读**的——worker 永远不会写配置
- 不写 → COW 页永远不复制 → 所有 worker 共享同一物理页
- 内存只分配一次，无论多少 worker

## 三、关键技术手段

### 3.1 不可变数组（IS_ARRAY_IMMUTABLE）

```c
// php_yaconf_hash_init() — 创建不可变持久数组
static void php_yaconf_hash_init(zval *zv, size_t size) {
    HashTable *ht = pemalloc(sizeof(HashTable), 1);  // persistent
    zend_hash_init(ht, size, NULL, ZVAL_PTR_DTOR, 1);

    // 标记不可变：引擎不会尝试修改，COW 页不会被复制
    GC_SET_REFCOUNT(ht, 2);
    GC_ADD_FLAGS(ht, IS_ARRAY_IMMUTABLE);
}
```

**IS_ARRAY_IMMUTABLE 的作用**：告诉 Zend 引擎这个数组不可变。当 worker 尝试写入时，引擎会分离（separate）数组而不是修改原数组——保护共享数据不被意外修改。

### 3.2 Interned Strings（驻留字符串）

```c
static zend_string* php_yaconf_str_persistent(char *str, size_t len) {
    zend_string *key = zend_string_init(str, len, 1);  // persistent
    GC_ADD_FLAGS(key, IS_STR_INTERNED | IS_STR_PERMANENT);
    return key;
}
```

**效果**：所有 worker 共享同一字符串。`"database.host"` 在内存中只有一份，不管多少 worker 引用它。

### 3.3 两阶段紧凑块压缩（Two-Phase Compaction）

```
解析阶段（分散分配）:
  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐
  │ HashTable│  │ string  │  │ HashTable│  │ string  │  ... 散落在堆上
  └─────────┘  └─────────┘  └─────────┘  └─────────┘

压缩阶段（单个连续块）:
  ┌──────────────────────────────────────────────────────┐
  │ str1 │ str2 │ HT1 │ HT1.data │ HT2 │ HT2.data │ ... │  连续内存块
  └──────────────────────────────────────────────────────┘
```

**双重收益**：
- 更少的独立分配 → 更低的内存开销
- 连续内存布局 → 更好的 CPU cache locality，worker COW 时触及的内存页更少

### 3.4 热重载（Auto-Reload）

```
每个 worker 的 RINIT（每个请求开始时）:
  │
  ├─ 检查 check_delay 节流（默认 300s）
  │   └─ 距上次检查 < check_delay → 跳过
  │
  ├─ stat 配置目录 → 比较 mtime
  │   └─ mtime 变了 → 重新扫描目录
  │
  ├─ 重新解析变更的文件 → 更新配置树
  │   └─ COW 页被复制，该 worker 获得独立副本
  │
  └─ 其他 worker 不受影响，继续用旧配置
      └─ 下次各自的 RINIT 检查时也会 reload
```

**关键设计**：
- 各 worker **独立检测、独立重载**，最终收敛到新配置
- 不需要跨进程通知——每个 worker 自己发现变更
- COW 保证 reload 期间其他 worker 不受影响

### 3.5 ZTS 限制

```c
#ifndef ZTS
    PHP_RINIT(yaconf),   // 热重载只在非 ZTS 模式注册
#else
    NULL,                // ZTS 模式不注册 RINIT
#endif
```

**原因**：ZTS 模式下多个请求共享同一进程空间，原地替换 zval 会有并发风险。热重载的 `check_delay`/`last_check`/`directory_mtime` 字段也只在非 ZTS 下存在。

## 四、数据流图

```
┌─────────────────────────────────────────────────────────────┐
│  FPM master（MINIT，fork workers 前）                        │
│                                                              │
│  1. 扫描 yaconf.directory 目录                               │
│  2. 解析所有 .ini 文件 → 临时内存树（emalloc）                │
│  3. 两阶段压缩 → 单个连续内存块（pemalloc，persistent）        │
│  4. 标记 IS_ARRAY_IMMUTABLE + interned strings               │
│                                                              │
│  fork() ─────────────────────────────────────────────        │
└─────────────────────────────────────────────────────────────┘
                          │
          ┌───────────────┼───────────────┐
          ▼               ▼               ▼
   ┌─────────────┐ ┌─────────────┐ ┌─────────────┐
   │  worker 1   │ │  worker 2   │ │  worker N   │
   │             │ │             │ │             │
   │ RINIT:      │ │ RINIT:      │ │ RINIT:      │
   │  check mtime│ │  check mtime│ │  check mtime│
   │  (节流)     │ │  (节流)     │ │  (节流)     │
   │             │ │             │ │             │
   │ Yaconf::get │ │ Yaconf::get │ │ Yaconf::get │
   │  ↓          │ │  ↓          │ │  ↓          │
   │ 哈希查找     │ │ 哈希查找     │ │ 哈希查找     │
   │ (ns 级)     │ │ (ns 级)     │ │ (ns 级)     │
   └─────────────┘ └─────────────┘ └─────────────┘
          │               │               │
          └───────────────┼───────────────┘
                          │
                    ┌─────┴─────┐
                    │ 共享内存页 │  ← OS COW，所有 worker 共享
                    │ (配置块)   │     同一物理内存
                    └───────────┘
```

## 五、与 beacon 的对比

| 维度 | Yaconf | beacon |
|------|--------|--------|
| **数据性质** | 只读配置（启动后不变） | 运行时状态（持续变化） |
| **共享机制** | fork COW（零代码） | sysv shm（显式共享内存） |
| **写入** | 不写（reload 时整体替换） | 每请求写（RINIT/RSHUTDOWN 自计数） |
| **一致性** | 最终一致（各 worker 独立 reload） | 强一致（双缓冲原子切换） |
| **内存分配** | pemalloc（PHP 堆，persistent） | shmget/shmat（sysv 共享内存） |
| **延迟** | 哈希查找（ns 级） | shm 直读（ns 级） |
| **热重载** | mtime 检查 + COW 页复制 | 治理 worker 写 shm + 双缓冲切换 |
| **ZTS** | 热重载不可用 | 不受影响（shm 是进程级） |

## 六、beacon 可借鉴的点

### 6.1 Interned Strings

beacon 的 `beacon_node_t` 用固定长度 `char[]`（紧凑布局），但 PHP 层返回的字符串可以考虑 interned：

```c
// beacon_api.c 中 pick() 返回节点信息时
// 当前：add_assoc_string(return_value, "host", node->host);
// 可优化：使用 interned string 减少内存占用
```

### 6.2 mtime 节流模式

Yaconf 的 `check_delay` + `last_check` 节流模式，beacon 已用类似模式：

```c
// beacon.c RINIT 中，每 100 请求检查一次治理 worker 存活
if (++BEACON_G(request_count) % 100 == 0) {
    beacon_governance_ensure_running();
}
```

### 6.3 `__debug_info` 暴露内部状态

Yaconf 的 `__debug_info` 显示存储地址和 COW 状态（`changed` 字段），beacon 的 `status()` 已有类似设计（`mode`/`cache_age_seconds`）。

### 6.4 紧凑块压缩的启示

Yaconf 把整个配置树压入单个连续内存块，beacon 的 shm 布局已经是紧凑的 C 结构体（packed binary），思想一致。beacon 的双缓冲设计比 Yaconf 的整体替换更精细——Yaconf 是"全量替换"，beacon 是"增量更新"。

## 七、一句话总结

> **Yaconf 的跨 worker 共享 = 不可变数据 + fork COW + interned strings + 两阶段紧凑块压缩。零共享内存代码，零系统调用，零锁。配置是只读的，COW 是完美的共享机制。**
