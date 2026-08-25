# Yac 跨 Worker 缓存共享 — 学习笔记

> 来源：https://github.com/laruence/yac
> 作者：Xinchen Hui (laruence@php.net)
> 版本：2.4.0
> 学习日期：2026-08-26

---

## 一、解决什么问题

PHP 需要进程内缓存（替代 APC 或本地 memcached），但 FPM worker 是独立进程——如何让所有 worker 共享同一份缓存，支持**运行时读写**？

**核心矛盾**：缓存是**可变的**（运行时持续写入），不能用 Yaconf 的 COW 方案（写入会复制页面，worker 间不可见）。需要显式共享内存 + 并发控制。

## 二、核心方案：共享内存 + 无锁读 + CAS 写

```
FPM master
  │
  │  MINIT（fork workers 前）
  │  └─ 分配共享内存（mmap MAP_ANON → /dev/zero → sysv shmget）
  │     ├─ Keys memory（哈希槽位表，固定大小）
  │     └─ Values memory（环形缓冲区，分段管理）
  │
  │  fork() ──────────────────────────────────────────────
  │
  ├─ worker 1 ─┐
  ├─ worker 2 ─┤  所有 worker 映射同一共享内存段
  ├─ worker 3 ─┤  读：无锁（哈希查找 + CRC 校验）
  └─ worker N ─┘  写：per-slot CAS 自旋锁（无全局锁）
```

**与 Yaconf 的本质区别**：Yac 的数据是**可变的**，worker 需要**写入**共享内存。COW 写入会复制页面导致 worker 间不可见，所以必须用显式共享内存。

## 三、共享内存分配

### 3.1 三级降级策略

```c
// 优先 mmap(MAP_ANON)，其次 mmap(/dev/zero)，最后 sysv shmget
// 编译时确定，运行时不可变
```

| 优先级 | 方式 | 特点 |
|--------|------|------|
| 1 | `mmap(MAP_ANON)` | 匿名映射，无文件依赖，最快 |
| 2 | `mmap(/dev/zero)` | 设备文件映射，兼容性好 |
| 3 | `shmget` (sysv) | 系统 V 共享内存，兜底方案 |

### 3.2 内存布局：两个独立池

```
┌─────────────────────────────────────────────────────────────┐
│  Segment 0（第一个段，同时承载全局状态 + 哈希槽位表）          │
│  ├─ yac_storage_globals（全局状态：hits/miss/kicks/fails）   │
│  ├─ segments[] 指针数组                                      │
│  └─ slots[]（哈希槽位表，yac_kv_key 数组）                    │
├─────────────────────────────────────────────────────────────┤
│  Segment 1..N（值存储段，环形缓冲区）                         │
│  ├─ segment 1: [pos][size][data...]                         │
│  ├─ segment 2: [pos][size][data...]                         │
│  └─ ...                                                      │
└─────────────────────────────────────────────────────────────┘
```

**Keys memory**（`yac.keys_memory_size`，默认 4M）：
- 固定大小哈希槽位表，4M ≈ 32K 槽位
- 每个 key 占一个槽位，查找最多探测 4 个候选槽位
- 槽位满时驱逐最久未访问的条目（LRU）

**Values memory**（`yac.values_memory_size`，默认 64M）：
- 分为 `segment_num` 个段，每段 `segment_size` 字节（≥ 4M）
- 环形缓冲区：写入推进游标，空间不按条目释放
- 游标回绕时旧值被覆盖，CRC 校验失败 → 转为 miss

## 四、核心数据结构

### 4.1 槽位（yac_kv_key）

```c
typedef struct {
    unsigned long h;           // 64-bit 哈希值
    unsigned int len;          // klen | vlen << 8（打包存储）
    unsigned int ttl;          // 过期时间戳
    unsigned int mutex;        // CAS 自旋锁（0=free, 1=locked）
    union {
        unsigned int flag;     // block 值：序列化类型 + 压缩标记
        unsigned int hits;     // embedded 值：命中计数
    } u1;
    union {
        struct { unsigned int crc; unsigned int size; };  // block 值
        unsigned long atime;   // embedded 值：访问时间
    } u2;
    yac_kv_val *val;           // 值指针（或 embedded 标记字）
    unsigned char key[48];     // 键（最大 48 字节）
} yac_kv_key;
```

**设计要点**：
- `mutex` 是 per-slot 的 CAS 锁，不是全局锁——不同 key 的写入可以并行
- `u1`/`u2` 是 union，根据存储形式（block vs embedded）复用内存
- `key[48]` 固定长度，避免变长分配

### 4.2 值块（yac_kv_val）

```c
typedef struct {
    unsigned int len;          // klen | vlen << 8
    unsigned int hits;         // 命中计数
    unsigned long atime;       // 最后访问时间
    char data[1];              // 值数据（变长）
} yac_kv_val;
```

### 4.3 Embedded 值（小值优化）

小值直接嵌入槽位的 `val` 指针字段，不分配值块：

```
val 指针的低 3 位作为标记（块分配 8 字节对齐，低 3 位恒为 0）：
  0x1 = NULL
  0x2 = TRUE
  0x3 = FALSE
  0x4 = LONG（zend_long 在高位）
  0x5 = SHORT_STR（≤7 字节）
  0x6 = EMPTY_ARRAY
```

**收益**：小值（NULL/bool/int/短字符串/空数组）零内存分配，零间接寻址。

## 五、并发控制：无锁读 + CAS 写

### 5.1 读路径（无锁）

```c
int yac_storage_find(const char *key, unsigned int len, ...) {
    hash = murmurhash2(key, len);
    for (i = 0; i < 4; i++) {           // 最多探测 4 个槽位
        p = &slots[h & slots_mask];
        if (!WRITEP(p)) break;           // CAS 尝试加锁（读也加锁！）
        k = *p;                          // 拷贝槽位内容
        READP(p);                        // 立即释放锁
        // ... 在锁外校验 k 的内容 ...
        if (k.h == hash && !memcmp(k.key, key, len)) {
            // 命中：CRC 校验值完整性
            if (k.crc == crc32(data)) {
                return 1;  // 返回数据拷贝
            }
        }
        h += seed & slots_mask;          // 二次哈希探测
    }
    return 0;  // miss
}
```

**关键**：读也加锁（`WRITEP`/`READP`），但锁的粒度是 per-slot 的 CAS 自旋锁，不是全局锁。锁的持有时间极短（拷贝槽位内容到栈上），然后释放锁在锁外做 CRC 校验和数据拷贝。

### 5.2 写路径（CAS 自旋锁）

```c
int yac_storage_update(const char *key, unsigned int len, ...) {
    // 1. 探测路径（最多 4 个槽位）
    // 2. 路径满 → 驱逐最久未访问的条目
    // 3. 填充新值到槽位
    // 4. CAS 提交：
    if (!WRITEP(p)) return 0;  // CAS 加锁失败 → 返回 false
    p->h = k.h;
    p->ttl = k.ttl;
    memcpy(p->key, k.key, len);
    p->val = k.val;
    READP(p);                   // 释放锁
    return 1;
}
```

**CAS 自旋锁实现**：

```c
// yac_atomic.h
#define YAC_CAS(lock, old, set)  __sync_bool_compare_and_swap(lock, old, set)

static inline int yac_slot_lock(unsigned int *me) {
    int retry = 0;
    while (!YAC_CAS(me, YAC_SLOT_FREE, YAC_SLOT_LOCKED)) {
        if (++retry == YAC_CAS_MAX_SPIN) return 0;  // 30 次自旋后放弃
        yac_cpu_relax();  // pause 指令，降低 CPU 功耗
    }
    return 1;
}

static inline void yac_slot_unlock(unsigned int *me) {
    __sync_lock_release(me);  // release store of 0
}
```

**关键特性**：
- **Per-slot 锁**：不同 key 的写入可以并行，吞吐量随 worker 数线性增长
- **自旋上限 30 次**：避免无限自旋，失败返回 false（调用方可重试）
- **CPU relax**：x86 `pause` / ARM `yield` 指令，降低自旋功耗

### 5.3 值分配（环形缓冲区 + CAS）

```c
static inline void *yac_allocator_alloc_algo2(unsigned long size, int hash) {
    current = hash & segments_num_mask;  // 按哈希选段
    segment = segments[current];
    pos = segment->pos;
    if ((seg_size - pos) >= size) {
        // CAS 推进游标
        if (YAC_CAS(&segment->pos, pos, pos + size)) {
            return segment->p + pos;
        }
    }
    // 段满 → 尝试其他段 → 全部满 → 游标回绕（recycle）
    segment->pos = 0;
    ++recycles;
}
```

**关键**：值分配也是 CAS 操作，多 worker 同时写不同段可以并行。

## 六、数据流图

```
┌─────────────────────────────────────────────────────────────┐
│  FPM master（MINIT，fork workers 前）                        │
│                                                              │
│  1. 分配共享内存（mmap MAP_ANON / /dev/zero / shmget）       │
│  2. 初始化全局状态（hits/miss/kicks/fails = 0）              │
│  3. 初始化哈希槽位表（memset 0）                              │
│  4. 初始化值段（pos = 0）                                    │
│                                                              │
│  fork() ─────────────────────────────────────────────        │
└─────────────────────────────────────────────────────────────┘
                          │
          ┌───────────────┼───────────────┐
          ▼               ▼               ▼
   ┌─────────────┐ ┌─────────────┐ ┌─────────────┐
   │  worker 1   │ │  worker 2   │ │  worker N   │
   │             │ │             │ │             │
   │ $yac->get() │ │ $yac->get() │ │ $yac->set() │
   │  ↓          │ │  ↓          │ │  ↓          │
   │ 哈希查找     │ │ 哈希查找     │ │ CAS 写入    │
   │ (无锁读)    │ │ (无锁读)    │ │ (per-slot)  │
   │  ↓          │ │  ↓          │ │  ↓          │
   │ CRC 校验    │ │ CRC 校验    │ │ 环形缓冲区   │
   │  ↓          │ │  ↓          │ │  CAS 分配   │
   │ 返回拷贝    │ │ 返回拷贝    │ │ 返回 true   │
   └─────────────┘ └─────────────┘ └─────────────┘
          │               │               │
          └───────────────┼───────────────┘
                          │
                    ┌─────┴─────┐
                    │ 共享内存段 │  ← mmap/shmget，所有 worker
                    │           │     映射同一物理内存
                    │ slots[]   │     （哈希槽位表）
                    │ segments[]│     （值环形缓冲区）
                    └───────────┘
```

## 七、与 beacon 的对比

| 维度 | Yac | beacon |
|------|-----|--------|
| **数据性质** | 可变缓存（读写频繁） | 可变状态（读多写少） |
| **共享机制** | mmap/shmget（显式共享内存） | sysv shmget（显式共享内存） |
| **并发控制** | per-slot CAS 自旋锁 | 双缓冲无锁读写 |
| **读路径** | 无锁（CAS 拷贝槽位 + 锁外校验） | 无锁（读激活 buffer） |
| **写路径** | CAS 自旋锁（per-slot，30 次自旋上限） | 写非激活 buffer + 原子切指针 |
| **内存布局** | 哈希槽位表 + 环形缓冲区段 | C 结构体数组（packed binary） |
| **一致性** | 松弛一致（CRC 校验防脏读） | 强一致（双缓冲原子切换） |
| **驱逐** | LRU（atime 最旧）+ 环形回绕 | 无驱逐（治理 worker 全量更新） |
| **值存储** | 序列化 + 压缩（LZ4） | C 结构体（零序列化） |
| **小值优化** | Embedded 值（零分配） | 固定长度 char[]（紧凑布局） |

## 八、beacon 可借鉴的点

### 8.1 Per-slot CAS 自旋锁

Yac 的 per-slot CAS 锁比全局锁并发度高得多。beacon 当前用双缓冲避免锁，但如果未来需要支持**多写入者**（多个治理 worker），可以借鉴 per-slot CAS：

```c
// beacon 当前：单写入者（治理 worker）+ 双缓冲
// 若未来多写入者：per-service CAS 锁
typedef struct {
    // ...
    unsigned int mutex;  // per-service CAS 锁
} beacon_service_t;
```

### 8.2 Embedded 值优化

Yac 的小值嵌入槽位（零分配），beacon 的心跳槽位已是固定大小（64 字节对齐），思想一致。beacon 的节点结构体也是固定长度 `char[]`，避免了变长分配。

### 8.3 CRC 校验防脏读

Yac 用 CRC32 校验值完整性（环形缓冲区回绕时旧值被覆盖，CRC 失败 → miss）。beacon 也用 CRC32 校验 shm 完整性（`beacon_shm_header_t.checksum`），思想一致。

### 8.4 哈希探测策略

Yac 用双哈希（MurmurHash2 + DJBX33A）最多探测 4 个槽位。beacon 的心跳槽位用 `pid % MAX` + 线性探测，更简单但足够（worker 数远小于槽位数）。

### 8.5 统计计数器

Yac 的全局统计（hits/miss/kicks/fails/recycles）是 lock-free 的（`++YAC_SG(hits)` 在共享内存上直接自增）。beacon 的 pool 自计数（`atomic_fetch_add(&pool_busy, 1)`）用 C11 stdatomic，更精确。

## 九、一句话总结

> **Yac 的跨 worker 缓存共享 = 共享内存（mmap/shmget）+ per-slot CAS 自旋锁 + 无锁读 + 环形缓冲区 + CRC 校验。不用全局锁，不同 key 的写入并行，吞吐量随 worker 数线性增长。**
