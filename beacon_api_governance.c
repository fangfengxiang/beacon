/* beacon_api_governance.c — Beacon\Governance PHP Userland API
 *
 * 职责：注册 Beacon\Governance 类，实现 storeNodes/commit/calcHealth 静态方法。
 *       CLI SAPI 专用——治理 worker 进程调用这些方法写 shm 和计算健康。
 *
 * 设计依据：docs/design/api-reference.md §1.2 治理 worker API
 *           docs/design/mvp.md §6.2 功能模块对应表
 * 业界对标：Swoole\Server / Swoole\Process 的命名空间类注册
 *
 * 错误处理：PHP 扩展惯例——返回 bool 表示成功/失败，不抛异常。
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php_beacon.h"

#include <string.h>
#include <time.h>

/* ---- 状态映射辅助函数 ----
 *
 * 与 beacon_api.c 中的映射函数逻辑一致，但独立定义（避免跨文件符号依赖）。
 * 节点状态字符串 → 数值码（storeNodes 用，PHP userland 传字符串，C 层需数值码）。
 */
static uint8_t string_to_node_status(const char *str)
{
    if (!str) return BEACON_NODE_STATUS_OK;
    if (strcmp(str, "ok") == 0)         return BEACON_NODE_STATUS_OK;
    if (strcmp(str, "degraded") == 0)   return BEACON_NODE_STATUS_DEGRADED;
    if (strcmp(str, "dead") == 0)       return BEACON_NODE_STATUS_DEAD;
    if (strcmp(str, "not_ready") == 0)  return BEACON_NODE_STATUS_NOT_READY;
    return BEACON_NODE_STATUS_OK; /* 未知字符串默认 OK */
}

/* 健康状态数值 → 字符串（calcHealth 用，C 层返回数值，PHP userland 需字符串） */
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

/* ---- arginfo 声明 ---- */

/* Beacon\Governance::storeNodes(string $service, array $nodes): bool */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_governance_storeNodes, 0, 2, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, service, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, nodes, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* Beacon\Governance::commit(): bool */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_governance_commit, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

/* Beacon\Governance::calcHealth(): array */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_governance_calcHealth, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* ---- 方法实现 ---- */

/* Beacon\Governance::storeNodes(string $service, array $nodes): bool
 *
 * 把 PHP 数组转为 C 结构体写入 shm（非激活 buffer）。
 * 治理 worker discover 后调用。
 *
 * $nodes 数组元素格式：
 *   ['id' => 'node-1', 'host' => '10.0.0.1', 'port' => 9000,
 *    'status' => 'ok', 'weight' => 1, 'methods' => 'getUser,createUser']
 */
PHP_METHOD(Beacon_Governance, storeNodes)
{
    char *service;
    size_t service_len;
    zval *nodes_arr;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "sa", &service, &service_len, &nodes_arr) == FAILURE) {
        RETURN_FALSE;
    }

    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm) {
        php_error_docref(NULL, E_WARNING, "shm unavailable, extension in degraded mode");
        RETURN_FALSE;
    }

    /* 限制节点数 */
    uint32_t count = zend_hash_num_elements(Z_ARRVAL_P(nodes_arr));
    if (count > BEACON_MAX_NODES_PER_SVC) {
        count = BEACON_MAX_NODES_PER_SVC;
    }

    /* 栈上数组（零分配，对标 beacon_service_select.c 的候选集模式） */
    beacon_node_t nodes[BEACON_MAX_NODES_PER_SVC];
    memset(nodes, 0, sizeof(nodes));

    /* 遍历 PHP 数组，转换为 C 结构体 */
    uint32_t i = 0;
    zval *entry;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(nodes_arr), entry) {
        if (i >= count) break;
        if (Z_TYPE_P(entry) != IS_ARRAY) continue;

        /* id (string) */
        zval *zid = zend_hash_str_find(Z_ARRVAL_P(entry), "id", sizeof("id") - 1);
        if (zid && Z_TYPE_P(zid) == IS_STRING) {
            strncpy(nodes[i].id, Z_STRVAL_P(zid), BEACON_MAX_NODE_ID_LEN - 1);
        }

        /* host (string) */
        zval *zhost = zend_hash_str_find(Z_ARRVAL_P(entry), "host", sizeof("host") - 1);
        if (zhost && Z_TYPE_P(zhost) == IS_STRING) {
            strncpy(nodes[i].host, Z_STRVAL_P(zhost), BEACON_MAX_HOST_LEN - 1);
        }

        /* port (int, 1-65535) */
        zval *zport = zend_hash_str_find(Z_ARRVAL_P(entry), "port", sizeof("port") - 1);
        if (zport && Z_TYPE_P(zport) == IS_LONG) {
            zend_long port_val = Z_LVAL_P(zport);
            if (port_val < 1 || port_val > 65535) {
                php_error_docref(NULL, E_WARNING,
                    "node port %lld out of range (1-65535), node skipped", (long long)port_val);
                continue;
            }
            nodes[i].port = (uint16_t)port_val;
        }

        /* status (string → numeric) */
        zval *zstatus = zend_hash_str_find(Z_ARRVAL_P(entry), "status", sizeof("status") - 1);
        if (zstatus && Z_TYPE_P(zstatus) == IS_STRING) {
            nodes[i].status = string_to_node_status(Z_STRVAL_P(zstatus));
        } else if (zstatus && Z_TYPE_P(zstatus) == IS_LONG) {
            /* 兼容整数 status（内部测试用） */
            nodes[i].status = (uint8_t)Z_LVAL_P(zstatus);
        } else {
            nodes[i].status = BEACON_NODE_STATUS_OK; /* 默认 OK */
        }

        /* weight (int, 默认 1，必须 >= 0) */
        zval *zweight = zend_hash_str_find(Z_ARRVAL_P(entry), "weight", sizeof("weight") - 1);
        if (zweight && Z_TYPE_P(zweight) == IS_LONG && Z_LVAL_P(zweight) >= 0) {
            nodes[i].weight = (uint16_t)Z_LVAL_P(zweight);
        } else {
            nodes[i].weight = 1; /* 默认权重 1 */
        }

        /* methods (string, 可选) */
        zval *zmethods = zend_hash_str_find(Z_ARRVAL_P(entry), "methods", sizeof("methods") - 1);
        if (zmethods && Z_TYPE_P(zmethods) == IS_STRING) {
            strncpy(nodes[i].methods, Z_STRVAL_P(zmethods), BEACON_MAX_METHODS_LEN - 1);
        }

        i++;
    } ZEND_HASH_FOREACH_END();

    /* 写入 shm 非激活 buffer */
    if (beacon_shm_store_nodes(service, nodes, i) != 0) {
        RETURN_FALSE;
    }

    RETURN_TRUE;
}

/* Beacon\Governance::commit(): bool
 *
 * 原子切 active buffer，FPM worker 立即可见新数据。
 * storeNodes 后调用。
 */
PHP_METHOD(Beacon_Governance, commit)
{
    if (zend_parse_parameters_none() == FAILURE) {
        RETURN_FALSE;
    }

    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm) {
        php_error_docref(NULL, E_WARNING, "shm unavailable, extension in degraded mode");
        RETURN_FALSE;
    }

    if (beacon_shm_commit() != 0) {
        RETURN_FALSE;
    }

    RETURN_TRUE;
}

/* Beacon\Governance::calcHealth(): array
 *
 * 读 shm 自计数，算 pool 级健康状态。
 * 治理 worker keepalive 前调用。
 *
 * 返回：['status' => 'ok', 'pool_busy' => 2, 'pool_idle' => 8,
 *        'pool_total' => 100, 'saturation' => 0.2, 'governance_alive' => true]
 */
PHP_METHOD(Beacon_Governance, calcHealth)
{
    if (zend_parse_parameters_none() == FAILURE) {
        RETURN_EMPTY_ARRAY();
    }

    /* 更新治理 worker 心跳：本 API 由治理 worker keepalive 周期调用，调用即心跳。
     * FPM worker 读侧（Beacon::status()）据此时间戳判断控制面存活。
     * 先写后算：calculate 读到的即最新心跳。 */
    beacon_shm_t *shm = BEACON_G(shm);
    if (shm) {
        shm->header.governance_alive = (uint64_t)time(NULL);
    }

    /* 调用领域服务层 */
    beacon_health_t h = beacon_health_calculate();

    array_init(return_value);
    add_assoc_string(return_value, "status", health_status_to_string(h.status));
    add_assoc_long(return_value, "pool_busy", (zend_long)h.pool_busy);
    add_assoc_long(return_value, "pool_idle", (zend_long)h.pool_idle);
    add_assoc_long(return_value, "pool_total", (zend_long)h.pool_total);
    add_assoc_double(return_value, "saturation", h.saturation);
    add_assoc_bool(return_value, "governance_alive", h.governance_alive ? 1 : 0);
}

/* ---- 方法表 ---- */
static const zend_function_entry governance_methods[] = {
    PHP_ME(Beacon_Governance, storeNodes, arginfo_governance_storeNodes, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Beacon_Governance, commit,     arginfo_governance_commit,     ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Beacon_Governance, calcHealth, arginfo_governance_calcHealth, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

/* ---- 注册 Beacon\Governance 类 ----
 *
 * 对标 Swoole\Server / Swoole\Process 的命名空间类注册。
 * PHP 命名空间在 C 层用下划线连接：Beacon\Governance → "Beacon\\Governance"。
 */
void beacon_register_governance_class(int module_number)
{
    (void)module_number; /* 类常量用 zend_declare_class_constant_*，不需要 module_number */

    zend_class_entry ce;
    INIT_CLASS_ENTRY_EX(ce, "Beacon\\Governance", sizeof("Beacon\\Governance") - 1, governance_methods);
    zend_class_entry *ce_ptr = zend_register_internal_class(&ce);
    (void)ce_ptr;
}
