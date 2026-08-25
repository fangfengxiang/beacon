/* beacon_api.c — Beacon PHP Userland API
 *
 * 职责：注册 Beacon 类，实现 pick/getInstances/ready/status/setOpt/
 *       deregister/reportHealth 静态方法。
 *
 * 设计依据：docs/design/api-reference.md §1.1 核心 API
 *           docs/design/mvp.md §6.2 功能模块对应表
 * 业界对标：Swoole (swoole_api.cc) / Yar (yar.c) / Redis (redis.c) 的类注册
 *
 * 错误处理：PHP 扩展惯例——返回 null/false 表示状态，不抛异常。
 *   - pick() 返回 ?array，null = 无可用节点
 *   - getInstances() 返回 array，空数组 = 无节点
 *   - setOpt() 返回 bool，false = 无效 key 或 value
 *   - 内部错误（shm 不可用）→ php_error_docref 警告 + 返回 false/null
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php_beacon.h"

#include <string.h>
#include <time.h>

/* ---- 运行时 LB 策略覆盖（setOpt 设置，pick 读取）----
 *
 * per-FPM-worker，不跨进程共享。
 * 0 = 未覆盖，使用 INI beacon.lb_strategy 默认值。
 * 非 0 = 使用 setOpt 设置的值。
 * 纳入模块全局结构体以支持 ZTS（线程安全构建）。
 */

/* ---- 状态映射辅助函数 ----
 *
 * C 层用数值码（BEACON_HEALTH_STATUS_* / BEACON_NODE_STATUS_*），
 * PHP userland 用字符串常量（Beacon::HEALTH_*）。
 * API 层负责数值↔字符串映射。
 *
 * 注意：健康状态码和节点状态码数值不同但映射到相同字符串集。
 * 对标 gRPC Health "SERVING"/"NOT_SERVING" 字符串状态。
 */

/* 健康状态数值 → 字符串（pool 级） */
static const char *health_status_to_string(uint8_t status)
{
    switch (status) {
        case BEACON_HEALTH_STATUS_NOT_READY: return "not_ready";
        case BEACON_HEALTH_STATUS_OK:       return "ok";
        case BEACON_HEALTH_STATUS_DEGRADED:  return "degraded";
        case BEACON_HEALTH_STATUS_DEAD:     return "dead";
        default:                             return "unknown";
    }
}

/* 节点状态数值 → 字符串（per-instance） */
static const char *node_status_to_string(uint8_t status)
{
    switch (status) {
        case BEACON_NODE_STATUS_OK:         return "ok";
        case BEACON_NODE_STATUS_DEGRADED:    return "degraded";
        case BEACON_NODE_STATUS_DEAD:        return "dead";
        case BEACON_NODE_STATUS_NOT_READY:   return "not_ready";
        default:                             return "unknown";
    }
}

/* 健康状态字符串 → 数值（reportHealth 用） */
static uint8_t string_to_health_status(const char *str)
{
    if (!str) return BEACON_HEALTH_STATUS_NOT_READY;
    if (strcmp(str, "ok") == 0)         return BEACON_HEALTH_STATUS_OK;
    if (strcmp(str, "degraded") == 0)   return BEACON_HEALTH_STATUS_DEGRADED;
    if (strcmp(str, "dead") == 0)       return BEACON_HEALTH_STATUS_DEAD;
    if (strcmp(str, "not_ready") == 0)  return BEACON_HEALTH_STATUS_NOT_READY;
    return BEACON_HEALTH_STATUS_NOT_READY; /* 未知字符串默认 NOT_READY */
}

/* ---- INI LB 策略字符串 → 数值 ---- */
static int beacon_parse_lb_strategy(const char *str)
{
    if (!str) return BEACON_LB_ROUND_ROBIN;
    if (strcmp(str, "round_robin") == 0) return BEACON_LB_ROUND_ROBIN;
    if (strcmp(str, "random") == 0)      return BEACON_LB_RANDOM;
    if (strcmp(str, "weighted") == 0)    return BEACON_LB_WEIGHTED;
    return BEACON_LB_ROUND_ROBIN; /* 默认 round_robin */
}

/* ---- 获取生效的 LB 策略 ----
 *
 * 优先级：pick() opts > setOpt 运行时覆盖 > INI 默认值
 */
static int get_effective_lb_strategy(zval *opts)
{
    /* 1. pick() opts 中的 OPT_LB_STRATEGY（整数 key，对标 Beacon::OPT_LB_STRATEGY = 6） */
    if (opts && Z_TYPE_P(opts) == IS_ARRAY) {
        zval *lb = zend_hash_index_find(Z_ARRVAL_P(opts), BEACON_OPT_LB_STRATEGY);
        if (lb && Z_TYPE_P(lb) == IS_LONG) {
            return (int)Z_LVAL_P(lb);
        }
    }

    /* 2. setOpt 运行时覆盖 */
    if (BEACON_G(runtime_lb_strategy) != 0) {
        return BEACON_G(runtime_lb_strategy);
    }

    /* 3. INI 默认值 */
    return beacon_parse_lb_strategy(BEACON_G(lb_strategy));
}

/* ---- arginfo 声明 ---- */

/* Beacon::pick(string $service, array $opts = []): ?array */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_beacon_pick, 0, 1, IS_ARRAY, 1)
    ZEND_ARG_TYPE_INFO(0, service, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, opts, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* Beacon::getInstances(string $service): array */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_beacon_getInstances, 0, 1, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, service, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* Beacon::ready(): bool */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_beacon_ready, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

/* Beacon::status(): array */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_beacon_status, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* Beacon::setOpt(int $key, mixed $value): bool */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_beacon_setOpt, 0, 2, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, key, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, value, IS_MIXED, 0)
ZEND_END_ARG_INFO()

/* Beacon::deregister(): bool */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_beacon_deregister, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

/* Beacon::reportHealth(array $health): bool */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_beacon_reportHealth, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, health, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* ---- 方法实现 ---- */

/* Beacon::pick(string $service, array $opts = []): ?array
 *
 * 内置 LB 选一个实例。返回 {id, host, port, status, weight, methods} 或 null。
 *
 * opts 支持：
 *   Beacon::OPT_EXCLUDE (11) — 已试节点 id 数组（failover 用）
 *   Beacon::OPT_LB_STRATEGY (6) — 临时覆盖 LB 策略
 *   Beacon::OPT_PREFER_HEALTHY (12) — bool，是否只选 HEALTHY（默认 true）
 */
PHP_METHOD(Beacon, pick)
{
    char *service;
    size_t service_len;
    zval *opts = NULL;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "s|a", &service, &service_len, &opts) == FAILURE) {
        RETURN_NULL();
    }

    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm) {
        php_error_docref(NULL, E_WARNING, "shm unavailable, extension in degraded mode");
        RETURN_NULL();
    }

    /* 解析 opts */
    const char *exclude_ids[BEACON_MAX_NODES_PER_SVC];
    uint32_t exclude_count = 0;
    bool prefer_healthy = true; /* 默认 true */
    int strategy = get_effective_lb_strategy(opts);

    if (opts && Z_TYPE_P(opts) == IS_ARRAY) {
        /* OPT_EXCLUDE = 11 */
        zval *exc = zend_hash_index_find(Z_ARRVAL_P(opts), BEACON_OPT_EXCLUDE);
        if (exc && Z_TYPE_P(exc) == IS_ARRAY) {
            zval *entry;
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(exc), entry) {
                if (exclude_count >= BEACON_MAX_NODES_PER_SVC) break;
                if (Z_TYPE_P(entry) == IS_STRING) {
                    exclude_ids[exclude_count++] = Z_STRVAL_P(entry);
                }
            } ZEND_HASH_FOREACH_END();
        }

        /* OPT_PREFER_HEALTHY = 12 */
        zval *ph = zend_hash_index_find(Z_ARRVAL_P(opts), BEACON_OPT_PREFER_HEALTHY);
        if (ph && Z_TYPE_P(ph) == IS_TRUE) {
            prefer_healthy = true;
        } else if (ph && Z_TYPE_P(ph) == IS_FALSE) {
            prefer_healthy = false;
        }
    }

    /* 调用领域服务层 */
    const beacon_node_t *node = beacon_select_pick(
        service, strategy, exclude_ids, exclude_count, prefer_healthy);

    if (!node) {
        RETURN_NULL();
    }

    /* 构建返回数组 */
    array_init(return_value);
    add_assoc_string(return_value, "id", node->id);
    add_assoc_string(return_value, "host", node->host);
    add_assoc_long(return_value, "port", node->port);
    add_assoc_string(return_value, "status", node_status_to_string(node->status));
    add_assoc_long(return_value, "weight", node->weight);
    if (node->methods[0] != '\0') {
        add_assoc_string(return_value, "methods", node->methods);
    }
}

/* Beacon::getInstances(string $service): array
 *
 * 取该服务全部健康实例（OK + DEGRADED），排除 DEAD/NOT_READY。
 */
PHP_METHOD(Beacon, getInstances)
{
    char *service;
    size_t service_len;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &service, &service_len) == FAILURE) {
        RETURN_EMPTY_ARRAY();
    }

    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm) {
        php_error_docref(NULL, E_WARNING, "shm unavailable, extension in degraded mode");
        RETURN_EMPTY_ARRAY();
    }

    beacon_node_t nodes[BEACON_MAX_NODES_PER_SVC];
    uint32_t count = 0;

    if (beacon_select_get_instances(service, nodes, &count) != 0) {
        RETURN_EMPTY_ARRAY();
    }

    array_init(return_value);
    for (uint32_t i = 0; i < count; i++) {
        zval node_arr;
        array_init(&node_arr);
        add_assoc_string(&node_arr, "id", nodes[i].id);
        add_assoc_string(&node_arr, "host", nodes[i].host);
        add_assoc_long(&node_arr, "port", nodes[i].port);
        add_assoc_string(&node_arr, "status", node_status_to_string(nodes[i].status));
        add_assoc_long(&node_arr, "weight", nodes[i].weight);
        if (nodes[i].methods[0] != '\0') {
            add_assoc_string(&node_arr, "methods", nodes[i].methods);
        }
        add_next_index_zval(return_value, &node_arr);
    }
}

/* Beacon::ready(): bool
 *
 * 标记本实例预热完成，健康从 NOT_READY 转 OK。
 * 实现：写 shm pool_ready = 1，治理 worker 下次 keepalive 时读到并转 OK。
 */
PHP_METHOD(Beacon, ready)
{
    if (zend_parse_parameters_none() == FAILURE) {
        RETURN_FALSE;
    }

    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm) {
        php_error_docref(NULL, E_WARNING, "shm unavailable, extension in degraded mode");
        RETURN_FALSE;
    }

    shm->header.pool_ready = 1;
    RETURN_TRUE;
}

/* Beacon::status(): array
 *
 * 返回当前 pool 状态（governance_pid、governance_alive、cache_age_seconds、mode 等）。
 */
PHP_METHOD(Beacon, status)
{
    if (zend_parse_parameters_none() == FAILURE) {
        RETURN_EMPTY_ARRAY();
    }

    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm) {
        array_init(return_value);
        add_assoc_bool(return_value, "enabled", BEACON_G(enabled));
        add_assoc_string(return_value, "mode", "degraded");
        add_assoc_string(return_value, "health", "not_ready");
        add_assoc_bool(return_value, "governance_alive", false);
        add_assoc_long(return_value, "governance_pid", 0);
        add_assoc_long(return_value, "cache_age_seconds", -1);
        return;
    }

    /* 读 pool 级健康状态 */
    beacon_health_t h = beacon_health_calculate();

    /* 计算 cache age（距上次节点表更新的秒数） */
    uint64_t now = (uint64_t)time(NULL);
    int64_t cache_age = (shm->header.last_update > 0 && now >= shm->header.last_update)
                        ? (int64_t)(now - shm->header.last_update)
                        : -1;

    /* 判定模式 */
    const char *mode = "normal";
    if (!h.governance_alive) {
        mode = "degraded";
    }

    array_init(return_value);
    add_assoc_bool(return_value, "enabled", BEACON_G(enabled));
    add_assoc_string(return_value, "mode", mode);
    add_assoc_string(return_value, "health", health_status_to_string(h.status));
    add_assoc_bool(return_value, "governance_alive", h.governance_alive);
    add_assoc_long(return_value, "governance_pid", (zend_long)shm->header.governance_pid);
    add_assoc_long(return_value, "cache_age_seconds", (zend_long)cache_age);
    add_assoc_bool(return_value, "pool_ready", shm->header.pool_ready ? 1 : 0);
    add_assoc_long(return_value, "pool_busy", (zend_long)h.pool_busy);
    add_assoc_long(return_value, "pool_idle", (zend_long)h.pool_idle);
    add_assoc_long(return_value, "pool_total", (zend_long)h.pool_total);
    add_assoc_double(return_value, "saturation", h.saturation);
}

/* Beacon::setOpt(int $key, mixed $value): bool
 *
 * 设置运行时选项。对标 Yar_Client::setOpt / curl_setopt / Redis::setOption。
 *
 * key 取值（Beacon::OPT_*）：
 *   1-5: 回调（callable|null）→ beacon_callback_set
 *   6:   LB 策略（int）→ 运行时覆盖
 *   7-10: 间隔/阈值（int）→ 更新全局状态
 *   11-12: pick() 专用，setOpt 不支持
 */
PHP_METHOD(Beacon, setOpt)
{
    zend_long key;
    zval *value;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "lz", &key, &value) == FAILURE) {
        RETURN_FALSE;
    }

    /* 按 key 分发 */
    if (key >= BEACON_OPT_ON_REGISTER && key <= BEACON_OPT_ON_WATCH) {
        /* 1-5: 回调设置 */
        int result = beacon_callback_set((beacon_callback_type_t)key, value);
        RETURN_BOOL(result == 0);
    }

    switch (key) {
        case BEACON_OPT_LB_STRATEGY:
            /* 6: LB 策略覆盖 */
            if (Z_TYPE_P(value) != IS_LONG) {
                php_error_docref(NULL, E_WARNING, "OPT_LB_STRATEGY expects int, got %s",
                                zend_get_type_by_const(Z_TYPE_P(value)));
                RETURN_FALSE;
            }
            BEACON_G(runtime_lb_strategy) = (int)Z_LVAL_P(value);
            RETURN_TRUE;

        case BEACON_OPT_KEEPALIVE_INTERVAL:
            /* 7: 保活间隔（秒，必须 > 0，防治理 worker 紧密循环） */
            if (Z_TYPE_P(value) != IS_LONG || Z_LVAL_P(value) <= 0) {
                php_error_docref(NULL, E_WARNING, "OPT_KEEPALIVE_INTERVAL expects positive int");
                RETURN_FALSE;
            }
            BEACON_G(keepalive_interval) = Z_LVAL_P(value);
            RETURN_TRUE;

        case BEACON_OPT_PULL_INTERVAL:
            /* 8: 拉取间隔（秒，必须 > 0） */
            if (Z_TYPE_P(value) != IS_LONG || Z_LVAL_P(value) <= 0) {
                php_error_docref(NULL, E_WARNING, "OPT_PULL_INTERVAL expects positive int");
                RETURN_FALSE;
            }
            BEACON_G(pull_interval) = Z_LVAL_P(value);
            RETURN_TRUE;

        case BEACON_OPT_HEARTBEAT_TTL:
            /* 9: 心跳 TTL（秒，必须 > 0） */
            if (Z_TYPE_P(value) != IS_LONG || Z_LVAL_P(value) <= 0) {
                php_error_docref(NULL, E_WARNING, "OPT_HEARTBEAT_TTL expects positive int");
                RETURN_FALSE;
            }
            BEACON_G(heartbeat_ttl) = Z_LVAL_P(value);
            RETURN_TRUE;

        case BEACON_OPT_HEALTH_DEAD_THRESHOLD:
            /* 10: dead 阈值（连续失败次数，必须 > 0） */
            if (Z_TYPE_P(value) != IS_LONG || Z_LVAL_P(value) <= 0) {
                php_error_docref(NULL, E_WARNING, "OPT_HEALTH_DEAD_THRESHOLD expects positive int");
                RETURN_FALSE;
            }
            BEACON_G(health_dead_threshold) = Z_LVAL_P(value);
            RETURN_TRUE;

        case BEACON_OPT_EXCLUDE:
        case BEACON_OPT_PREFER_HEALTHY:
            /* 11-12: pick() 专用，setOpt 不支持 */
            php_error_docref(NULL, E_WARNING,
                "OPT_EXCLUDE and OPT_PREFER_HEALTHY are pick()-only options, use Beacon::pick($service, $opts) instead");
            RETURN_FALSE;

        default:
            php_error_docref(NULL, E_WARNING, "unknown option key: %lld", (long long)key);
            RETURN_FALSE;
    }
}

/* Beacon::deregister(): bool
 *
 * 手动注销。写 shm deregister_flag = 1，治理 worker 下次 tick 时读到并执行 on_deregister。
 */
PHP_METHOD(Beacon, deregister)
{
    if (zend_parse_parameters_none() == FAILURE) {
        RETURN_FALSE;
    }

    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm) {
        php_error_docref(NULL, E_WARNING, "shm unavailable, extension in degraded mode");
        RETURN_FALSE;
    }

    shm->header.deregister_flag = 1;
    RETURN_TRUE;
}

/* Beacon::reportHealth(array $health): bool
 *
 * 业务主动报健康。写 shm business_health_status，治理 worker keepalive 时合并。
 * $health 数组至少包含 'status' 键（Beacon::HEALTH_* 字符串常量）。
 */
PHP_METHOD(Beacon, reportHealth)
{
    zval *health;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "a", &health) == FAILURE) {
        RETURN_FALSE;
    }

    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm) {
        php_error_docref(NULL, E_WARNING, "shm unavailable, extension in degraded mode");
        RETURN_FALSE;
    }

    /* 提取 status 字段 */
    zval *status_zv = zend_hash_str_find(Z_ARRVAL_P(health), "status", sizeof("status") - 1);
    if (!status_zv || Z_TYPE_P(status_zv) != IS_STRING) {
        php_error_docref(NULL, E_WARNING, "health array must contain 'status' string field");
        RETURN_FALSE;
    }

    /* 字符串 → 数值码 */
    uint8_t status_code = string_to_health_status(Z_STRVAL_P(status_zv));
    shm->header.business_health_status = status_code;

    RETURN_TRUE;
}

/* ---- 方法表 ---- */
static const zend_function_entry beacon_methods[] = {
    PHP_ME(Beacon, pick,         arginfo_beacon_pick,         ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Beacon, getInstances, arginfo_beacon_getInstances, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Beacon, ready,        arginfo_beacon_ready,        ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Beacon, status,       arginfo_beacon_status,       ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Beacon, setOpt,       arginfo_beacon_setOpt,       ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Beacon, deregister,   arginfo_beacon_deregister,   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Beacon, reportHealth, arginfo_beacon_reportHealth, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

/* ---- 健康状态字符串值（类常量 HEALTH_* 的值）----
 *
 * 对标 gRPC Health "SERVING"/"NOT_SERVING"、K8s probe passing/warning/critical。
 * 用字符串而非整数，便于 PHP userland 可读。
 */
#define BEACON_HEALTH_NOT_READY_STR  "not_ready"
#define BEACON_HEALTH_OK_STR         "ok"
#define BEACON_HEALTH_DEGRADED_STR   "degraded"
#define BEACON_HEALTH_DEAD_STR       "dead"

/* ---- 注册 Beacon 类 ----
 *
 * 对标 Swoole swoole_register_classes / Yar yar_register_classes。
 * INIT_CLASS_ENTRY + zend_register_internal_class + zend_declare_class_constant_*。
 *
 * 常量注册为类常量（Beacon::OPT_* / Beacon::LB_* / Beacon::HEALTH_*），
 * 对标 Yar Yar_Client::OPT_PACKAGER / Redis Redis::OPT_* 的类常量方式。
 * PHP userland 用 Beacon::OPT_EXCLUDE 访问，而非 Beacon\OPT_EXCLUDE 全局常量。
 */
void beacon_register_beacon_class(int module_number)
{
    (void)module_number; /* 类常量用 zend_declare_class_constant_*，不需要 module_number */

    zend_class_entry ce;
    INIT_CLASS_ENTRY_EX(ce, "Beacon", sizeof("Beacon") - 1, beacon_methods);
    zend_class_entry *ce_ptr = zend_register_internal_class(&ce);

    /* 选项 key 常量（整数）—— setOpt() 的 key，pick() 的 opts 数组 key */
    zend_declare_class_constant_long(ce_ptr, "OPT_ON_REGISTER",        sizeof("OPT_ON_REGISTER")-1,        BEACON_OPT_ON_REGISTER);
    zend_declare_class_constant_long(ce_ptr, "OPT_ON_KEEPALIVE",       sizeof("OPT_ON_KEEPALIVE")-1,       BEACON_OPT_ON_KEEPALIVE);
    zend_declare_class_constant_long(ce_ptr, "OPT_ON_DISCOVER",       sizeof("OPT_ON_DISCOVER")-1,        BEACON_OPT_ON_DISCOVER);
    zend_declare_class_constant_long(ce_ptr, "OPT_ON_DEREGISTER",      sizeof("OPT_ON_DEREGISTER")-1,      BEACON_OPT_ON_DEREGISTER);
    zend_declare_class_constant_long(ce_ptr, "OPT_ON_WATCH",           sizeof("OPT_ON_WATCH")-1,           BEACON_OPT_ON_WATCH);
    zend_declare_class_constant_long(ce_ptr, "OPT_LB_STRATEGY",        sizeof("OPT_LB_STRATEGY")-1,        BEACON_OPT_LB_STRATEGY);
    zend_declare_class_constant_long(ce_ptr, "OPT_KEEPALIVE_INTERVAL", sizeof("OPT_KEEPALIVE_INTERVAL")-1, BEACON_OPT_KEEPALIVE_INTERVAL);
    zend_declare_class_constant_long(ce_ptr, "OPT_PULL_INTERVAL",      sizeof("OPT_PULL_INTERVAL")-1,      BEACON_OPT_PULL_INTERVAL);
    zend_declare_class_constant_long(ce_ptr, "OPT_HEARTBEAT_TTL",      sizeof("OPT_HEARTBEAT_TTL")-1,      BEACON_OPT_HEARTBEAT_TTL);
    zend_declare_class_constant_long(ce_ptr, "OPT_HEALTH_DEAD_THRESHOLD", sizeof("OPT_HEALTH_DEAD_THRESHOLD")-1, BEACON_OPT_HEALTH_DEAD_THRESHOLD);
    zend_declare_class_constant_long(ce_ptr, "OPT_EXCLUDE",            sizeof("OPT_EXCLUDE")-1,            BEACON_OPT_EXCLUDE);
    zend_declare_class_constant_long(ce_ptr, "OPT_PREFER_HEALTHY",     sizeof("OPT_PREFER_HEALTHY")-1,     BEACON_OPT_PREFER_HEALTHY);

    /* LB 策略枚举（整数） */
    zend_declare_class_constant_long(ce_ptr, "LB_ROUND_ROBIN", sizeof("LB_ROUND_ROBIN")-1, BEACON_LB_ROUND_ROBIN);
    zend_declare_class_constant_long(ce_ptr, "LB_RANDOM",      sizeof("LB_RANDOM")-1,      BEACON_LB_RANDOM);
    zend_declare_class_constant_long(ce_ptr, "LB_WEIGHTED",    sizeof("LB_WEIGHTED")-1,    BEACON_LB_WEIGHTED);

    /* 健康状态枚举（字符串） */
    zend_declare_class_constant_string(ce_ptr, "HEALTH_NOT_READY", sizeof("HEALTH_NOT_READY")-1, BEACON_HEALTH_NOT_READY_STR);
    zend_declare_class_constant_string(ce_ptr, "HEALTH_OK",       sizeof("HEALTH_OK")-1,       BEACON_HEALTH_OK_STR);
    zend_declare_class_constant_string(ce_ptr, "HEALTH_DEGRADED",  sizeof("HEALTH_DEGRADED")-1,  BEACON_HEALTH_DEGRADED_STR);
    zend_declare_class_constant_string(ce_ptr, "HEALTH_DEAD",      sizeof("HEALTH_DEAD")-1,      BEACON_HEALTH_DEAD_STR);
}
