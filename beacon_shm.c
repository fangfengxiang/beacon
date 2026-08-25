/* beacon_shm.c — sysv 共享内存双缓冲实现
 *
 * 设计依据：docs/design/shm-design.md
 * 核心机制：
 *   - sysv shmget/shmat 分配跨进程共享内存（master + workers + governance worker 共享）
 *   - 双缓冲无锁读写（治理 worker 写非激活 buffer，FPM worker 读激活 buffer）
 *   - per-worker 心跳槽位（RINIT/RSHUTDOWN 写，治理 worker 校准）
 *   - C11 atomic 自计数（pool_busy/idle/total）
 *   - CRC32 校验 + 内存屏障
 *
 * 业界对标：Swoole Table（shm + 无锁）、Swoole Atomic（shm + atomic）、Linux RCU（读者无锁）
 *
 * 防御原则：shm 任何操作失败都不崩溃，仅记 error 日志，扩展降级模式继续运行。
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php_beacon.h"

#include <sys/ipc.h>
#include <sys/shm.h>
#include <string.h>
#include <time.h>
#include <signal.h>  /* kill(pid, 0) */
#include <errno.h>
#include <stdio.h>

#ifdef HAVE_ZLIB_CRC32
#include <zlib.h>
#endif

/* ---- 常量 ---- */
#define BEACON_SHM_FALLBACK_KEY    0xBEAC0000  /* ftok 失败的兜底固定 key */
#define BEACON_SHM_FTOK_PATH       "/tmp"      /* ftok 路径 */
#define BEACON_SHM_FTOK_PROJ_ID    'B'         /* ftok proj_id 兜底 */
#define BEACON_HEARTBEAT_TIMEOUT   60          /* 心跳超时秒数（超过视为死进程） */
#define BEACON_BUSY_DRIFT_THRESHOLD 2          /* busy 计数偏差阈值（超过则强制校准） */

/* ---- CRC32 实现（无 zlib 时用内置表驱动实现）----
 *
 * 对标 zlib crc32() / Swoole swCRC32。
 * 表驱动，O(n) 复杂度，用于 header 校验和。
 */
static uint32_t crc32_table[256];
static int crc32_table_initialized = 0;

static void crc32_table_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
        crc32_table[i] = crc;
    }
    crc32_table_initialized = 1;
}

uint32_t beacon_shm_crc32(const void *data, size_t len)
{
#ifdef HAVE_ZLIB_CRC32
    return (uint32_t)crc32(0L, (const Bytef *)data, len);
#else
    if (!crc32_table_initialized) {
        crc32_table_init();
    }
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
#endif
}

/* ---- shm key 生成 ----
 *
 * beacon.shm_key INI 配置优先（用户显式指定），
 * 未配置（=0）用 ftok 生成（基于 service_name，保证同服务稳定）。
 */
static key_t beacon_shm_generate_key(void)
{
    if (BEACON_G(shm_key) != 0) {
        return (key_t)BEACON_G(shm_key);
    }

    /* ftok 兜底：用 service_name 首字符 + 固定 proj_id
     * 对标 Swoole shm_key 用 ftok 生成 */
    const char *svc = BEACON_G(service_name);
    int proj_id = (svc && svc[0]) ? (int)(unsigned char)svc[0] : BEACON_SHM_FTOK_PROJ_ID;
    key_t key = ftok(BEACON_SHM_FTOK_PATH, proj_id);
    if (key == (key_t)-1) {
        beacon_log(BEACON_LOG_WARN, "ftok failed, falling back to fixed key");
        return BEACON_SHM_FALLBACK_KEY;
    }
    return key;
}

/* ---- shm 初始化（MINIT 调用）---- */
int beacon_shm_init(void)
{
    key_t key = beacon_shm_generate_key();
    size_t shm_size = sizeof(beacon_shm_t);

    /* 尝试获取已存在的 shm 段（FPM reload 场景） */
    int shmid = shmget(key, shm_size, 0666);
    if (shmid == -1) {
        /* 不存在，创建新段 */
        shmid = shmget(key, shm_size, IPC_CREAT | IPC_EXCL | 0666);
        if (shmid == -1) {
            beacon_log(BEACON_LOG_ERROR, "shmget failed (key=0x%x, size=%zu): %s",
                       (unsigned)key, shm_size, strerror(errno));
            return -1;
        }
        BEACON_G(is_shm_owner) = 1;
    } else {
        BEACON_G(is_shm_owner) = 0;
    }

    /* attach */
    void *addr = shmat(shmid, NULL, 0);
    if (addr == (void *)-1) {
        beacon_log(BEACON_LOG_ERROR, "shmat failed (shmid=%d): %s", shmid, strerror(errno));
        return -1;
    }

    BEACON_G(shm_id) = shmid;
    BEACON_G(shm) = (beacon_shm_t *)addr;

    /* 初始化 header（仅创建者初始化，避免覆盖已有数据） */
    if (BEACON_G(is_shm_owner)) {
        memset(BEACON_G(shm), 0, sizeof(beacon_shm_t));
        beacon_shm_header_t *h = &BEACON_G(shm)->header;
        h->magic = BEACON_SHM_MAGIC;
        h->shm_version = 1;
        atomic_store(&h->active, 0);  /* 默认 buffer A 激活 */
        atomic_store(&h->pool_busy, 0);
        atomic_store(&h->pool_idle, 0);
        atomic_store(&h->pool_total, 0);
        h->pool_ready = 0;
        h->governance_alive = (uint64_t)time(NULL);
        h->last_update = 0;
        h->checksum = beacon_shm_crc32(&BEACON_G(shm)->buffer_a[0],
                                        sizeof(beacon_service_t) * BEACON_MAX_SERVICES);
        beacon_log(BEACON_LOG_INFO, "shm created (key=0x%x, shmid=%d, size=%zu)",
                   (unsigned)key, shmid, shm_size);
    } else {
        /* 校验 magic（防止 key 冲突误用其他应用的 shm） */
        if (BEACON_G(shm)->header.magic != BEACON_SHM_MAGIC) {
            beacon_log(BEACON_LOG_ERROR, "shm magic mismatch (expected 0x%x, got 0x%x), key conflict?",
                       BEACON_SHM_MAGIC, BEACON_G(shm)->header.magic);
            shmdt(BEACON_G(shm));
            BEACON_G(shm) = NULL;
            BEACON_G(shm_id) = -1;
            return -1;
        }
        beacon_log(BEACON_LOG_INFO, "shm attached (key=0x%x, shmid=%d, existing)",
                   (unsigned)key, shmid);
    }

    return 0;
}

/* ---- shm 销毁（MSHUTDOWN 调用）---- */
void beacon_shm_destroy(void)
{
    if (BEACON_G(shm) != NULL) {
        shmdt(BEACON_G(shm));
        BEACON_G(shm) = NULL;
    }

    /* 仅创建者删除段（防止其他 FPM pool 还在用） */
    if (BEACON_G(is_shm_owner) && BEACON_G(shm_id) >= 0) {
        if (shmctl(BEACON_G(shm_id), IPC_RMID, NULL) == -1) {
            beacon_log(BEACON_LOG_WARN, "shmctl IPC_RMID failed (shmid=%d): %s",
                       BEACON_G(shm_id), strerror(errno));
        } else {
            beacon_log(BEACON_LOG_INFO, "shm removed (shmid=%d)", BEACON_G(shm_id));
        }
    }
    BEACON_G(shm_id) = -1;
    BEACON_G(is_shm_owner) = 0;
}

/* ---- per-worker 心跳槽位注册（RINIT 调用）----
 *
 * 按 pid % MAX 哈希找槽位，冲突时线性探测。
 * 写 pid + last_rinit + busy=1。
 */
int beacon_shm_worker_register(pid_t pid)
{
    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm) return -1;

    uint64_t now = (uint64_t)time(NULL);
    int start = (int)((unsigned)pid % BEACON_MAX_WORKERS);

    for (int i = 0; i < BEACON_MAX_WORKERS; i++) {
        int slot = (start + i) % BEACON_MAX_WORKERS;
        beacon_worker_slot_t *w = &shm->header.workers[slot];

        uint32_t existing_pid = w->pid;
        if (existing_pid == 0 || existing_pid == (uint32_t)pid) {
            w->pid = (uint32_t)pid;
            w->last_rinit = now;
            w->busy = 1;
            /* 原子自计数（快速路径） */
            atomic_fetch_add(&shm->header.pool_busy, 1);
            return 0;
        }
    }

    /* 槽位全满（256 个 worker，极端情况）—— 覆盖第一个槽位 */
    beacon_log(BEACON_LOG_WARN, "worker slots full, overwriting slot %d", start);
    beacon_worker_slot_t *w = &shm->header.workers[start];
    w->pid = (uint32_t)pid;
    w->last_rinit = now;
    w->busy = 1;
    atomic_fetch_add(&shm->header.pool_busy, 1);
    return 0;
}

/* ---- per-worker 心跳槽位释放（RSHUTDOWN 调用）---- */
int beacon_shm_worker_release(pid_t pid)
{
    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm) return -1;

    uint64_t now = (uint64_t)time(NULL);
    int start = (int)((unsigned)pid % BEACON_MAX_WORKERS);

    for (int i = 0; i < BEACON_MAX_WORKERS; i++) {
        int slot = (start + i) % BEACON_MAX_WORKERS;
        beacon_worker_slot_t *w = &shm->header.workers[slot];

        if (w->pid == (uint32_t)pid) {
            w->busy = 0;
            w->last_rinit = now;
            /* 原子自计数 */
            atomic_fetch_sub(&shm->header.pool_busy, 1);
            atomic_fetch_add(&shm->header.pool_total, 1);
            return 0;
        }
    }

    /* 槽位未找到（可能已被校准清零）—— 仅减计数 */
    atomic_fetch_sub(&shm->header.pool_busy, 1);
    atomic_fetch_add(&shm->header.pool_total, 1);
    return 0;
}

/* ---- 治理 worker 写节点表（写非激活 buffer）---- */
int beacon_shm_store_nodes(const char *service, const beacon_node_t *nodes, uint32_t count)
{
    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm || !service) return -1;

    /* 选非激活 buffer */
    uint8_t inactive = (uint8_t)(1 - atomic_load(&shm->header.active));
    beacon_service_t *buf = (inactive == 0) ? shm->buffer_a : shm->buffer_b;

    /* 找到对应 service slot（按 name 匹配，空 slot 兜底） */
    beacon_service_t *target = NULL;
    int empty_slot = -1;
    for (int i = 0; i < BEACON_MAX_SERVICES; i++) {
        if (buf[i].name[0] == '\0') {
            if (empty_slot < 0) empty_slot = i;
            continue;
        }
        if (strncmp(buf[i].name, service, BEACON_MAX_SVC_NAME_LEN - 1) == 0) {
            target = &buf[i];
            break;
        }
    }
    if (!target) {
        if (empty_slot < 0) {
            beacon_log(BEACON_LOG_ERROR, "service slots full, cannot store %s", service);
            return -1;
        }
        target = &buf[empty_slot];
        memset(target, 0, sizeof(beacon_service_t));
        strncpy(target->name, service, BEACON_MAX_SVC_NAME_LEN - 1);
    }

    /* 标记正在写 */
    target->writing = 1;
    __sync_synchronize();

    /* 写节点数据 */
    uint32_t copy_count = (count > BEACON_MAX_NODES_PER_SVC) ? BEACON_MAX_NODES_PER_SVC : count;
    for (uint32_t i = 0; i < copy_count; i++) {
        memcpy(&target->nodes[i], &nodes[i], sizeof(beacon_node_t));
    }
    /* 清空多余槽位（节点数减少时） */
    for (uint32_t i = copy_count; i < BEACON_MAX_NODES_PER_SVC; i++) {
        memset(&target->nodes[i], 0, sizeof(beacon_node_t));
    }

    target->node_count = copy_count;
    target->version++;
    target->writing = 0;

    return 0;
}

/* ---- 治理 worker 原子切 active buffer（commit）---- */
int beacon_shm_commit(void)
{
    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm) return -1;

    /* 更新 last_update 时间戳 */
    shm->header.last_update = (uint64_t)time(NULL);

    /* 重算 checksum（非激活 buffer，即将成为激活 buffer） */
    uint8_t inactive = (uint8_t)(1 - atomic_load(&shm->header.active));
    beacon_service_t *buf = (inactive == 0) ? shm->buffer_a : shm->buffer_b;
    shm->header.checksum = beacon_shm_crc32(buf, sizeof(beacon_service_t) * BEACON_MAX_SERVICES);

    /* 内存屏障：确保上面的写操作在切 active 之前对其他 CPU 可见
     * 对标 Linux kernel smp_mb() / Swoole SW_LOCK_CPU_RELAX */
    __sync_synchronize();

    /* 原子切 active */
    atomic_store(&shm->header.active, inactive);

    /* 再一次屏障：确保 active 切换对读者可见 */
    __sync_synchronize();

    return 0;
}

/* ---- FPM worker 读服务节点表（读激活 buffer，CRC 校验失败回退备份）---- */
const beacon_service_t *beacon_shm_read_service(const char *service)
{
    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm || !service) return NULL;

    uint8_t active = (uint8_t)atomic_load(&shm->header.active);
    beacon_service_t *primary = (active == 0) ? shm->buffer_a : shm->buffer_b;
    beacon_service_t *backup  = (active == 0) ? shm->buffer_b : shm->buffer_a;

    /* 在 primary 中查找 */
    for (int i = 0; i < BEACON_MAX_SERVICES; i++) {
        if (primary[i].name[0] == '\0') continue;
        if (strncmp(primary[i].name, service, BEACON_MAX_SVC_NAME_LEN - 1) == 0) {
            if (primary[i].node_count > 0 && primary[i].version > 0) {
                return &primary[i];
            }
            break;
        }
    }

    /* primary 损坏/空，读备份 buffer（双缓冲互救） */
    beacon_log(BEACON_LOG_WARN, "primary buffer for %s empty/corrupted, trying backup", service);
    for (int i = 0; i < BEACON_MAX_SERVICES; i++) {
        if (backup[i].name[0] == '\0') continue;
        if (strncmp(backup[i].name, service, BEACON_MAX_SVC_NAME_LEN - 1) == 0) {
            if (backup[i].node_count > 0 && backup[i].version > 0) {
                beacon_log(BEACON_LOG_INFO, "recovered %s from backup buffer", service);
                return &backup[i];
            }
            break;
        }
    }

    beacon_log(BEACON_LOG_ERROR, "both buffers corrupted for %s, no data", service);
    return NULL;
}

/* ---- 治理 worker 校准 busy 计数（扫描心跳槽位，kill(pid,0) 检测死进程）----
 *
 * 设计依据：docs/design/shm-design.md §4.3
 * FPM worker 被 kill -9 / OOM / segfault 时 RSHUTDOWN 不执行，pool_busy 永久泄漏。
 * 治理 worker 每 keepalive 前扫描槽位，kill(pid,0) 检测死进程并清零。
 */
int beacon_shm_calibrate_busy(void)
{
    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm) return 0;

    uint64_t now = (uint64_t)time(NULL);
    int actual_busy = 0;
    int stale_slots = 0;

    for (int i = 0; i < BEACON_MAX_WORKERS; i++) {
        beacon_worker_slot_t *w = &shm->header.workers[i];
        if (w->pid == 0) continue;

        /* kill(pid, 0) 不发送信号，只检查进程是否存在 */
        if (kill((pid_t)w->pid, 0) != 0 && errno == ESRCH) {
            /* 进程已死，清零槽位 */
            w->pid = 0;
            w->busy = 0;
            w->last_rinit = 0;
            stale_slots++;
            continue;
        }

        /* 心跳超时：超过 BEACON_HEARTBEAT_TIMEOUT 秒未更新视为死进程 */
        if (now > w->last_rinit && (now - w->last_rinit) > BEACON_HEARTBEAT_TIMEOUT) {
            w->pid = 0;
            w->busy = 0;
            stale_slots++;
            continue;
        }

        if (w->busy) actual_busy++;
    }

    /* 偏差超过 2 或发现 stale slots，强制校准 */
    unsigned shm_busy = atomic_load(&shm->header.pool_busy);
    int diff = (int)shm_busy - actual_busy;
    if (diff < 0) diff = -diff;

    if (diff > BEACON_BUSY_DRIFT_THRESHOLD || stale_slots > 0) {
        beacon_log(BEACON_LOG_WARN,
                   "busy counter drift: shm=%u, actual=%d, stale=%d, calibrating",
                   shm_busy, actual_busy, stale_slots);
        atomic_store(&shm->header.pool_busy, (unsigned)actual_busy);
    }

    return actual_busy;
}
