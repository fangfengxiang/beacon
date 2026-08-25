/* beacon_service_select.c — 节点选取与负载均衡
 *
 * 职责：从 shm 读服务节点表 + status 过滤 + exclude 列表 + LB 选址。
 *
 * 设计依据：docs/design/api-reference.md §1.1-1.3（pick/getInstances API）
 *           docs/design/spi.md §三（健康状态过滤）
 *           docs/design/mvp.md §6.2（功能模块对应表）
 * 业界对标：nginx upstream（round_robin/least_conn/ip_hash）
 *           Envoy LB（round_robin/random/weighted）
 *           gRPC client-side LB（round_robin/pick_first）
 *
 * 三种 LB 策略：
 *   round_robin — 进程内计数器取模（per-FPM-worker，不跨进程共享）
 *   random      — rand() 取模
 *   weighted    — 按 node.weight 加权随机
 *
 * 节点过滤逻辑：
 *   1. 排除 status == DEAD（不接流量）
 *   2. 排除 status == NOT_READY（启动中，不接流量）
 *   3. 排除 exclude 列表中的节点 id（failover 用）
 *   4. prefer_healthy == true 时：优先 OK，DEGRADED 排后
 *
 * 返回值：指向 shm 的指针（零拷贝），调用方不应释放。
 * 生命周期：shm 数据在 commit 前不变（治理 worker 写非激活 buffer）。
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php_beacon.h"

#include <stdlib.h>
#include <string.h>

/* 节点状态码（BEACON_NODE_STATUS_*）和 LB 策略码（BEACON_LB_*）
 * 在 php_beacon.h 中定义，供 C 层共享。 */

/* ---- 进程内 round-robin 计数器 ----
 *
 * per-FPM-worker，不跨进程共享。理由：
 *   - 跨进程共享需要 shm 原子操作，增加请求路径开销
 *   - 每个 worker 独立轮询在统计上仍接近均匀分布（大数定律）
 *   - 业界对标：nginx worker 各自维护 round-robin 计数器
 */
static uint32_t rr_counter = 0;

/* ---- 候选节点数组（栈上，零分配）----
 *
 * 候选数不会超过 BEACON_MAX_NODES_PER_SVC（64），用固定大小数组避免 emalloc。
 * 对标设计原则：请求路径零 I/O + 零内存分配。
 */
typedef struct {
    const beacon_node_t *nodes[BEACON_MAX_NODES_PER_SVC];
    uint32_t count;
} beacon_candidate_set_t;

/* ---- 检查节点是否在 exclude 列表中 ---- */
static bool is_excluded(const beacon_node_t *node,
                        const char *const *exclude, uint32_t exclude_count)
{
    if (!exclude || exclude_count == 0) return false;
    for (uint32_t i = 0; i < exclude_count; i++) {
        if (exclude[i] && strncmp(node->id, exclude[i], BEACON_MAX_NODE_ID_LEN) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- 过滤节点 ----
 *
 * 两阶段过滤：
 *   阶段 1：收集 OK 节点（prefer_healthy 优先）
 *   阶段 2：如果 prefer_healthy 且阶段 1 有结果，直接返回；否则收集 DEGRADED 节点
 *
 * 排除：DEAD、NOT_READY、exclude 列表
 */
static void filter_nodes(const beacon_service_t *svc,
                         const char *const *exclude, uint32_t exclude_count,
                         bool prefer_healthy,
                         beacon_candidate_set_t *out)
{
    out->count = 0;
    if (!svc) return;

    /* 阶段 1：收集 OK 节点 */
    for (uint32_t i = 0; i < svc->node_count && i < BEACON_MAX_NODES_PER_SVC; i++) {
        const beacon_node_t *n = &svc->nodes[i];
        if (n->status != BEACON_NODE_STATUS_OK) continue;
        if (is_excluded(n, exclude, exclude_count)) continue;
        if (n->id[0] == '\0' || n->host[0] == '\0') continue;  /* 跳过空节点 */
        out->nodes[out->count++] = n;
    }

    /* 阶段 2：如果 prefer_healthy 且有 OK 节点，直接返回；
     *        否则补充 DEGRADED 节点 */
    if (prefer_healthy && out->count > 0) {
        return;
    }

    for (uint32_t i = 0; i < svc->node_count && i < BEACON_MAX_NODES_PER_SVC; i++) {
        const beacon_node_t *n = &svc->nodes[i];
        if (n->status != BEACON_NODE_STATUS_DEGRADED) continue;
        if (is_excluded(n, exclude, exclude_count)) continue;
        if (n->id[0] == '\0' || n->host[0] == '\0') continue;
        out->nodes[out->count++] = n;
    }
}

/* ---- round-robin 策略 ----
 *
 * 进程内计数器取模。对标 nginx upstream round_robin。
 */
static const beacon_node_t *lb_round_robin(beacon_candidate_set_t *cands)
{
    if (cands->count == 0) return NULL;
    uint32_t idx = rr_counter % cands->count;
    rr_counter++;
    return cands->nodes[idx];
}

/* ---- random 策略 ----
 *
 * rand() 取模。对标 Envoy random LB。
 * 不播种——PHP 进程启动时可能已播种，LB 不需要密码学安全随机性。
 */
static const beacon_node_t *lb_random(beacon_candidate_set_t *cands)
{
    if (cands->count == 0) return NULL;
    uint32_t idx = (uint32_t)(rand() % (int)cands->count);
    return cands->nodes[idx];
}

/* ---- weighted 加权随机策略 ----
 *
 * 按节点 weight 加权随机。对标 nginx weight=N、Envoy weighted。
 * 权重为 0 的节点不参与选取。
 */
static const beacon_node_t *lb_weighted(beacon_candidate_set_t *cands)
{
    if (cands->count == 0) return NULL;

    /* 计算权重总和 */
    uint32_t total_weight = 0;
    for (uint32_t i = 0; i < cands->count; i++) {
        uint16_t w = cands->nodes[i]->weight;
        if (w == 0) w = 1;  /* weight=0 视为 1，避免被忽略 */
        total_weight += w;
    }

    if (total_weight == 0) {
        /* 所有节点 weight=0，退化为 random */
        return lb_random(cands);
    }

    /* 加权随机：随机数 r ∈ [0, total_weight)，遍历累加权重直到 > r */
    uint32_t r = (uint32_t)(rand() % (int)total_weight);
    uint32_t cumulative = 0;
    for (uint32_t i = 0; i < cands->count; i++) {
        uint16_t w = cands->nodes[i]->weight;
        if (w == 0) w = 1;
        cumulative += w;
        if (r < cumulative) {
            return cands->nodes[i];
        }
    }

    /* 兜底（浮点精度边界，理论上不会到这） */
    return cands->nodes[cands->count - 1];
}

/* ---- 选取一个节点（Beacon::pick() 调）---- */
const beacon_node_t *beacon_select_pick(
    const char *service,
    int strategy,
    const char *const *exclude,
    uint32_t exclude_count,
    bool prefer_healthy)
{
    if (!service) return NULL;

    /* 读 shm 激活 buffer */
    const beacon_service_t *svc = beacon_shm_read_service(service);
    if (!svc) return NULL;

    /* 过滤节点 */
    beacon_candidate_set_t cands;
    filter_nodes(svc, exclude, exclude_count, prefer_healthy, &cands);

    if (cands.count == 0) return NULL;

    /* 按策略选取 */
    switch (strategy) {
        case BEACON_LB_ROUND_ROBIN:
            return lb_round_robin(&cands);
        case BEACON_LB_RANDOM:
            return lb_random(&cands);
        case BEACON_LB_WEIGHTED:
            return lb_weighted(&cands);
        default:
            /* 未知策略，退化为 round-robin */
            return lb_round_robin(&cands);
    }
}

/* ---- 获取全部健康节点（Beacon::getInstances() 调）---- */
int beacon_select_get_instances(
    const char *service,
    beacon_node_t *out_nodes,
    uint32_t *out_count)
{
    if (out_count) *out_count = 0;
    if (!service || !out_nodes || !out_count) return -1;

    /* 读 shm 激活 buffer */
    const beacon_service_t *svc = beacon_shm_read_service(service);
    if (!svc) return -1;

    /* 拷贝 OK + DEGRADED 节点（排除 DEAD/NOT_READY） */
    uint32_t count = 0;
    for (uint32_t i = 0; i < svc->node_count && i < BEACON_MAX_NODES_PER_SVC; i++) {
        const beacon_node_t *n = &svc->nodes[i];
        if (n->status == BEACON_NODE_STATUS_DEAD) continue;
        if (n->status == BEACON_NODE_STATUS_NOT_READY) continue;
        if (n->id[0] == '\0' || n->host[0] == '\0') continue;
        memcpy(&out_nodes[count], n, sizeof(beacon_node_t));
        count++;
    }

    *out_count = count;
    return 0;
}
