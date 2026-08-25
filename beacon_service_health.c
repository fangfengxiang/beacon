/* beacon_service_health.c — pool 级健康状态计算
 *
 * 职责：读 shm 自计数 + 心跳槽位校准 + 治理 worker 存活检测，
 *       算 pool 级健康状态（NOT_READY/OK/DEGRADED/DEAD）。
 *
 * 设计依据：docs/design/spi.md §三 健康检查器
 *           docs/design/shm-design.md §4.4 校准触发时机
 *           docs/design/architecture.md §5.2 稳态后台流
 * 业界对标：gRPC Health Checking Protocol（grpc.health.v1.Health）
 *           K8s liveness/readiness probe（passing/warning/critical）
 *           Envoy health check（healthy/degraded/unhealthy）
 *
 * 健康状态判定优先级（从高到低）：
 *   1. pool_ready == 0          → NOT_READY（预热未完成）
 *   2. 治理 worker 不存活         → DEGRADED（控制面断了，数据面仍活）
 *   3. 饱和度 >= 0.9             → DEGRADED（接近饱和）
 *   4. 兜底                       → OK
 *   5. shm 不可用                 → NOT_READY（降级模式）
 *
 * 关键决策：治理 worker 挂了报 DEGRADED 而非 DEAD
 *   理由：治理 worker 是控制面，FPM worker 是数据面。治理层挂了 FPM 仍能
 *   服务请求（用 shm 缓存），报 DEAD 会导致误摘。报 DEGRADED 降权更合理。
 *   对标 K8s：kubelet 挂了 Pod 不被立即摘除（grace period）。
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php_beacon.h"

#include <time.h>
#include <string.h>

/* ---- 饱和度阈值常量 ----
 *
 * 90% busy 意味着 pool 接近饱和，新请求可能排队。
 * 留 10% 余量给突发流量。对标 Envoy panic threshold（50% 节点不健康时全量路由）。
 * 定义为常量消除魔数（0.9 不自文档化）。
 */
#define BEACON_SATURATION_DEGRADED_THRESHOLD 0.9

/* 饱和度阈值常量在下方定义（BEACON_SATURATION_DEGRADED_THRESHOLD）。
 * 健康状态数值码（BEACON_HEALTH_STATUS_*）和节点状态码（BEACON_NODE_STATUS_*）
 * 在 php_beacon.h 中定义，供 C 层共享。 */

/* ---- 计算 pool 级健康状态 ----
 *
 * 治理 worker keepalive tick 中调用（docs/design/shm-design.md §4.4）：
 *   1. 先调 beacon_shm_calibrate_busy() 校准自计数
 *   2. 读校准后的 pool_busy 算饱和度
 *   3. 按优先级判定健康状态
 *
 * 返回 beacon_health_t 结构体，含 status + metrics（供 keepalive 回调携带）。
 */
beacon_health_t beacon_health_calculate(void)
{
    beacon_health_t health;
    memset(&health, 0, sizeof(health));
    health.status = BEACON_HEALTH_STATUS_NOT_READY;

    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm) {
        /* shm 不可用（降级模式）——返回 NOT_READY，所有 metrics 为 0 */
        return health;
    }

    /* 1. 先校准 busy 计数（扫描心跳槽位，kill(pid,0) 检测死进程） */
    (void)beacon_shm_calibrate_busy();

    /* 2. 读校准后的自计数 */
    unsigned busy  = atomic_load(&shm->header.pool_busy);
    unsigned idle  = atomic_load(&shm->header.pool_idle);
    unsigned total = atomic_load(&shm->header.pool_total);

    health.pool_busy  = busy;
    health.pool_idle  = idle;
    health.pool_total = total;

    /* 3. 治理 worker 存活检测 */
    uint64_t now = (uint64_t)time(NULL);
    uint64_t ttl = (uint64_t)BEACON_G(heartbeat_ttl);
    /* governance_alive == 0 表示从未活跃（启动初期），视为不存活 */
    if (shm->header.governance_alive > 0 && now >= shm->header.governance_alive) {
        health.governance_alive = (now - shm->header.governance_alive) < ttl;
    } else if (shm->header.governance_alive > 0) {
        /* 时间回拨（now < governance_alive），视为存活（保守） */
        health.governance_alive = true;
    } else {
        health.governance_alive = false;
    }

    /* 4. 饱和度计算 */
    unsigned pool_size = busy + idle;
    if (pool_size > 0) {
        health.saturation = (double)busy / (double)pool_size;
    } else {
        /* pool_size == 0：没有 idle worker 信息，用 busy 估算 */
        health.saturation = (busy > 0) ? 1.0 : 0.0;
    }

    /* 5. 按优先级判定健康状态 */
    if (!shm->header.pool_ready) {
        /* 预热未完成（Beacon::ready() 未调）——不接流量 */
        health.status = BEACON_HEALTH_STATUS_NOT_READY;
    } else if (!health.governance_alive) {
        /* 治理 worker 不存活——控制面断了，数据面仍活，报 DEGRADED 不报 DEAD */
        health.status = BEACON_HEALTH_STATUS_DEGRADED;
    } else if (health.saturation >= BEACON_SATURATION_DEGRADED_THRESHOLD) {
        /* 饱和度过高——接近饱和，降权 */
        health.status = BEACON_HEALTH_STATUS_DEGRADED;
    } else {
        /* 一切正常 */
        health.status = BEACON_HEALTH_STATUS_OK;
    }

    return health;
}
