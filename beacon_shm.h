/* beacon_shm.h — sysv 共享内存 C 结构体定义
 *
 * 设计依据：docs/design/shm-design.md §二 C 结构体定义
 * 核心决策：
 *   - packed 固定布局（跨进程一致，零反序列化）
 *   - 双缓冲无锁（RCU 思路，读者只读不锁）
 *   - per-worker 心跳槽位 64B 对齐（防 false sharing）
 *   - C11 atomic_int 自计数（无 pthread 锁）
 *
 * 业界对标：Swoole Table（列式固定 size + 偏移访问）、Linux percpu（cache line 对齐）
 */

#ifndef BEACON_SHM_H
#define BEACON_SHM_H

#include <stdint.h>
#include <stdatomic.h>
#include <sys/types.h>  /* pid_t */

/* ---- 容量上限（对标 Swoole Table 的 size 配置）---- */
#define BEACON_MAX_SERVICES         16    /* 最大服务数 */
#define BEACON_MAX_NODES_PER_SVC    64    /* 每服务最大节点数 */
#define BEACON_MAX_NODE_ID_LEN      64    /* 实例 ID 最大长度 */
#define BEACON_MAX_HOST_LEN         64    /* host 最大长度 */
#define BEACON_MAX_METHODS_LEN      256   /* 方法列表最大长度（逗号分隔） */
#define BEACON_MAX_WORKERS          256   /* per-worker 心跳槽位数 */
#define BEACON_MAX_SVC_NAME_LEN     64    /* 服务名最大长度 */

/* ---- 节点结构体（packed，≈ 392 bytes）----
 *
 * 对标 Swoole Table 的列定义（固定 size + 偏移访问）。
 * packed 确保跨进程布局一致（不同编译器/平台对齐不同）。
 */
typedef struct __attribute__((packed)) {
    char     id[BEACON_MAX_NODE_ID_LEN];      /* 实例 ID */
    char     host[BEACON_MAX_HOST_LEN];       /* host */
    uint16_t port;                             /* port */
    uint8_t  status;                           /* 0=OK, 1=DEGRADED, 2=DEAD, 3=NOT_READY */
    uint16_t weight;                           /* 权重 */
    char     methods[BEACON_MAX_METHODS_LEN];  /* 逗号分隔方法名 */
    uint32_t consecutive_failures;             /* 预留：熔断失败计数 */
    uint32_t last_failure_time;                /* 预留：上次失败时间 */
} beacon_node_t;

/* ---- 服务结构体（packed，≈ 25KB）---- */
typedef struct __attribute__((packed)) {
    char     name[BEACON_MAX_SVC_NAME_LEN];   /* 服务名 */
    uint32_t node_count;                       /* 当前有效节点数 */
    uint32_t version;                          /* 版本号（防脏读，每次写自增） */
    uint8_t  writing;                          /* 治理 worker 正在写（0=空闲，1=写入中） */
    beacon_node_t nodes[BEACON_MAX_NODES_PER_SVC];
} beacon_service_t;

/* ---- per-worker 心跳槽位（64 字节对齐，防 false sharing）----
 *
 * 对标 Linux kernel struct percpu / DPDK rte_ring 的 cache line 对齐。
 * 每个 slot 独占一个 cache line（x86/ARM 通常 64B），原子操作永远 L1 hit。
 *
 * 布局：
 *   offset 0-3:   pid (4B)
 *   offset 4-7:   _pad0 (4B，对齐 last_rinit 到 8B 边界)
 *   offset 8-15:  last_rinit (8B, uint64_t 时间戳)
 *   offset 16:    busy (1B)
 *   offset 17-63: padding (47B，填满 64B cache line)
 */
typedef struct __attribute__((aligned(64))) {
    uint32_t pid;              /* 4 bytes,  offset 0-3 */
    uint32_t _pad0;            /* 4 bytes,  offset 4-7（对齐 last_rinit 到 8B） */
    uint64_t last_rinit;       /* 8 bytes,  offset 8-15 */
    uint8_t  busy;             /* 1 byte,   offset 16 */
    uint8_t  padding[47];      /* 47 bytes, offset 17-63 */
} beacon_worker_slot_t;        /* 总大小 64 字节，对齐 64 */

/* ---- SHM Header ----
 *
 * 承载：双缓冲 active 指针、pool 级原子自计数、治理 worker 心跳、per-worker 槽位。
 *
 * 不使用 packed：header 含 atomic_uint 字段，packed 会移除对齐导致 ARM64 上原子访问
 * 未对齐（SIGBUS 或未定义行为）。header 在同平台同编译器进程间共享，布局天然一致。
 * workers 数组显式 aligned(64) 保证 cache line 对齐（防 false sharing）。
 */
#define BEACON_SHM_MAGIC 0x42454143  /* 'BEAC' — 校验 shm 段是否为本扩展创建 */

typedef struct {
    uint32_t magic;                         /* 'BEAC' = 0x42454143 */
    uint32_t shm_version;                   /* 结构体版本（兼容性，当前 1） */
    atomic_uint active;                     /* 双缓冲：0=A, 1=B（原子读写，防撕裂） */
    uint32_t checksum;                      /* CRC32（header + active buffer） */

    pid_t    governance_pid;                /* 治理 worker PID */

    /* Pool 级自计数（FPM worker 写，治理 worker 读） */
    atomic_uint pool_busy;                  /* 当前 busy worker 数 */
    atomic_uint pool_idle;                  /* 当前 idle worker 数 */
    atomic_uint pool_total;                 /* 累计处理请求数 */
    uint8_t  pool_ready;                    /* 0=NOT_READY, 1=READY（Beacon::ready() 写） */
    uint8_t  deregister_flag;               /* FPM worker 请求注销（Beacon::deregister() 写） */

    /* 治理 worker 心跳（治理 worker 写，FPM worker 读） */
    uint64_t governance_alive;              /* 治理 worker 上次活跃时间戳 */
    uint64_t last_update;                   /* 节点表上次更新时间戳 */

    /* 业务健康状态（FPM worker 写，治理 worker 读）
     * Beacon::reportHealth() 写，治理 worker keepalive 时合并（取最差）。
     * 0=未报告（默认），值同 BEACON_HEALTH_STATUS_* 数值码。
     */
    uint8_t  business_health_status;

    /* per-worker 心跳槽位（16KB），显式 cache line 对齐 */
    beacon_worker_slot_t workers[BEACON_MAX_WORKERS] __attribute__((aligned(64)));

    char     reserved[127];                 /* 对齐 + 预留扩展（减 1 字节补偿 business_health_status）*/
} beacon_shm_header_t;

/* ---- SHM 总布局（双缓冲）---- */
typedef struct {
    beacon_shm_header_t header;             /* Header（含心跳槽位） */
    beacon_service_t buffer_a[BEACON_MAX_SERVICES];  /* Buffer A */
    beacon_service_t buffer_b[BEACON_MAX_SERVICES];  /* Buffer B */
} beacon_shm_t;

/* ---- 编译期大小校验（防布局意外变化）----
 *
 * _Static_assert 在编译期失败则报错，确保结构体大小符合设计。
 * 对标 Linux kernel BUILD_BUG_ON / _Static_assert。
 */
_Static_assert(sizeof(beacon_worker_slot_t) == 64,
    "beacon_worker_slot_t must be exactly 64 bytes (cache line alignment)");
_Static_assert(_Alignof(beacon_worker_slot_t) == 64,
    "beacon_worker_slot_t must be aligned to 64 bytes (prevent false sharing)");
_Static_assert(sizeof(beacon_node_t) > 0,
    "beacon_node_t size must be positive");
_Static_assert(offsetof(beacon_shm_header_t, workers) > 0,
    "workers array must have valid offset");

/* ---- shm 读写接口（beacon_shm.c 实现）---- */

/* 初始化 shm（MINIT 调用）：shmget + shmat + header 初始化
 * 返回 0 成功 / -1 失败（不崩溃，降级模式） */
int  beacon_shm_init(void);

/* 销毁 shm（MSHUTDOWN 调用）：shmdt + shmctl(IPC_RMID) */
void beacon_shm_destroy(void);

/* per-worker 心跳槽位注册（RINIT 调用）：按 pid%256 哈希找槽位，写 pid+time+busy=1
 * 返回 0 成功 / -1 失败（shm 不可用） */
int  beacon_shm_worker_register(pid_t pid);

/* per-worker 心跳槽位释放（RSHUTDOWN 调用）：找槽位写 busy=0
 * 返回 0 成功 / -1 失败 */
int  beacon_shm_worker_release(pid_t pid);

/* 治理 worker 写节点表（写非激活 buffer）
 * service: 服务名，nodes: 节点数组，count: 节点数
 * 返回 0 成功 / -1 失败 */
int  beacon_shm_store_nodes(const char *service, const beacon_node_t *nodes, uint32_t count);

/* 治理 worker 原子切 active buffer（commit）
 * 内存屏障 + 切 active，FPM worker 立即可见新数据
 * 返回 0 成功 / -1 失败 */
int  beacon_shm_commit(void);

/* FPM worker 读服务节点表（读激活 buffer，CRC 校验失败回退备份）
 * service: 服务名
 * 返回 beacon_service_t* 或 NULL（双缓冲都不可用） */
const beacon_service_t *beacon_shm_read_service(const char *service);

/* 治理 worker 校准 busy 计数（扫描心跳槽位，kill(pid,0) 检测死进程）
 * 返回校准后的实际 busy 数 */
int  beacon_shm_calibrate_busy(void);

/* 计算 header CRC32 校验和 */
uint32_t beacon_shm_crc32(const void *data, size_t len);

#endif /* BEACON_SHM_H */
