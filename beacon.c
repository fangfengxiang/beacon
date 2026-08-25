/* beacon.c — php-beacon-extension module entry
 *
 * 模块入口：MINIT/MSHUTDOWN/RINIT/RSHUTDOWN 钩子，注册类/常量/INI。
 *
 * 设计依据：docs/design/mvp.md §6.2 功能模块对应表
 * 业界对标：Swoole (swoole.c) / Yar (yar.c) 的模块入口组织
 *
 * 生命周期约束（PHP-FPM 进程模型决定）：
 *   MINIT     — FPM master 执行（fork workers 前），无 VM，只做最小初始化
 *   RINIT     — FPM worker per-request 执行，只做原子操作（shm inc + 心跳槽位）
 *   RSHUTDOWN — FPM worker per-request 执行，只做原子操作（shm dec + 心跳槽位）
 *   MSHUTDOWN — FPM master 执行，kill 治理 worker + 清理 shm
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php_beacon.h"

/* ---- 全局结构体实例（非 ZTS 模式）---- */
ZEND_DECLARE_MODULE_GLOBALS(beacon)

/* ---- INI 指令注册（对标 Yar / curl 扩展的 PHP_INI_BEGIN）----
 *
 * 8 个核心 INI 指令 + 日志配置项。
 * 默认值符合 docs/design/api-reference.md §二。
 * 零配置模式：仅配 beacon.enabled=1 + beacon.service_name="calc" 即可运行。
 */
PHP_INI_BEGIN()
    STD_PHP_INI_BOOLEAN("beacon.enabled",          "0",           PHP_INI_SYSTEM, OnUpdateBool,   enabled,              zend_beacon_globals, beacon_globals)
    STD_PHP_INI_ENTRY("beacon.service_name",       "",            PHP_INI_SYSTEM, OnUpdateString, service_name,         zend_beacon_globals, beacon_globals)
    STD_PHP_INI_ENTRY("beacon.advertise_host",     "",            PHP_INI_SYSTEM, OnUpdateString, advertise_host,       zend_beacon_globals, beacon_globals)
    STD_PHP_INI_ENTRY("beacon.advertise_host_env", "",            PHP_INI_SYSTEM, OnUpdateString, advertise_host_env,   zend_beacon_globals, beacon_globals)
    STD_PHP_INI_ENTRY("beacon.advertise_port",     "",            PHP_INI_SYSTEM, OnUpdateString, advertise_port,       zend_beacon_globals, beacon_globals)
    STD_PHP_INI_ENTRY("beacon.registry_endpoint",  "",            PHP_INI_SYSTEM, OnUpdateString, registry_endpoint,    zend_beacon_globals, beacon_globals)
    STD_PHP_INI_ENTRY("beacon.governance_bin",     "",            PHP_INI_SYSTEM, OnUpdateString, governance_bin,       zend_beacon_globals, beacon_globals)
    STD_PHP_INI_ENTRY("beacon.governance_script",  "",            PHP_INI_SYSTEM, OnUpdateString, governance_script,    zend_beacon_globals, beacon_globals)
    STD_PHP_INI_ENTRY("beacon.keepalive_interval", "3",           PHP_INI_SYSTEM, OnUpdateLong,   keepalive_interval,   zend_beacon_globals, beacon_globals)
    STD_PHP_INI_ENTRY("beacon.pull_interval",      "2",           PHP_INI_SYSTEM, OnUpdateLong,   pull_interval,        zend_beacon_globals, beacon_globals)
    STD_PHP_INI_ENTRY("beacon.heartbeat_ttl",      "15",          PHP_INI_SYSTEM, OnUpdateLong,   heartbeat_ttl,        zend_beacon_globals, beacon_globals)
    STD_PHP_INI_ENTRY("beacon.health_dead_threshold","3",         PHP_INI_SYSTEM, OnUpdateLong,   health_dead_threshold, zend_beacon_globals, beacon_globals)
    STD_PHP_INI_ENTRY("beacon.lb_strategy",        "round_robin", PHP_INI_SYSTEM, OnUpdateString, lb_strategy,          zend_beacon_globals, beacon_globals)
    STD_PHP_INI_ENTRY("beacon.shm_key",            "0",           PHP_INI_SYSTEM, OnUpdateLong,   shm_key,              zend_beacon_globals, beacon_globals)
    STD_PHP_INI_ENTRY("beacon.log_file",           "",            PHP_INI_SYSTEM, OnUpdateString, log_file,             zend_beacon_globals, beacon_globals)
    STD_PHP_INI_ENTRY("beacon.log_level",          "warn",        PHP_INI_SYSTEM, OnUpdateString, log_level,            zend_beacon_globals, beacon_globals)
PHP_INI_END()

/* ---- 全局结构体初始化/清理（ZEND_GINIT/GSHUTDOWN 回调）---- */

/* GINIT：进程启动时清零全局结构体（对标 Swoole php_swoole_init_globals） */
static void php_beacon_init_globals(zend_beacon_globals *beacon_globals)
{
    memset(beacon_globals, 0, sizeof(zend_beacon_globals));
    beacon_globals->shm_id = -1;
    beacon_globals->shm = NULL;
    beacon_globals->is_shm_owner = 0;
    beacon_globals->governance_pid = 0;
    beacon_globals->governance_restart_count = 0;
    beacon_globals->request_count = 0;
    beacon_globals->runtime_lb_strategy = 0;
}

/* ---- 生命周期钩子 ---- */

/* MINIT：FPM master 执行（fork workers 前）
 *
 * 只做最小初始化（对标 Swoole swoole_module_init）：
 *   1. 注册常量
 *   2. 初始化 shm（失败不崩溃，降级模式）
 *   3. spawn 治理 worker（Phase 3 补充）
 *
 * 防御：MINIT 段错误会导致 master 崩溃，只做最小逻辑。
 */
PHP_MINIT_FUNCTION(beacon)
{
    ZEND_INIT_MODULE_GLOBALS(beacon, php_beacon_init_globals, NULL);
    REGISTER_INI_ENTRIES();

    /* 注册 PHP userland 类（Beacon, Beacon\Governance）—— Phase 4
     * 类常量（OPT_*, LB_*, HEALTH_*）随类注册，对标 Yar/Redis 类常量方式 */
    beacon_register_classes(module_number);

    /* 初始化日志（根据 INI log_file/log_level） */
    beacon_log_init_globals();

    /* 初始化 shm（失败不崩溃，降级模式） */
    if (BEACON_G(enabled)) {
        if (beacon_shm_init() != 0) {
            beacon_log(BEACON_LOG_ERROR, "shm init failed, extension running in degraded mode");
            /* 不返回 FAILURE——扩展降级，FPM 仍可服务请求 */
        } else {
            beacon_log(BEACON_LOG_INFO, "beacon extension initialized, shm ready");
        }

        /* 初始化回调存储 */
        beacon_callback_init();

        /* spawn 治理 worker（仅 FPM master，非治理 worker 自身）
         *
         * 环境变量 guard：子进程 MINIT 检查 BEACON_GOVERNANCE_WORKER=1，
         * 跳过 spawn 防止无限 fork 循环。
         * 对标 systemd ConditionPathIsSymbolicLink、nginx env guard。
         */
        if (!beacon_is_governance_worker()) {
            pid_t gov_pid = beacon_governance_spawn();
            if (gov_pid < 0) {
                beacon_log(BEACON_LOG_ERROR, "governance worker spawn failed, running without governance");
                /* 不返回 FAILURE——扩展降级，FPM 仍可服务请求 */
            }
        }
    }

    return SUCCESS;
}

/* MSHUTDOWN：FPM master 执行
 *
 * 1. kill 治理 worker（Phase 3 补充）
 * 2. 清理 shm
 */
PHP_MSHUTDOWN_FUNCTION(beacon)
{
    if (BEACON_G(enabled)) {
        /* 关闭治理 worker（SIGTERM + waitpid + SIGKILL fallback）
         * 仅 FPM master 执行，治理 worker 自身不执行（避免自杀） */
        if (!beacon_is_governance_worker()) {
            beacon_governance_shutdown();
        }

        /* 清理回调存储 */
        beacon_callback_cleanup();

        beacon_shm_destroy();
    }

    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

/* RINIT：FPM worker per-request 执行
 *
 * 只做原子操作（对标 Swoole RINIT 的轻量化）：
 *   1. 心跳槽位写 pid + time + busy=1
 *   2. pool_busy 原子自增
 *
 * 防御：RINIT 段错误会导致 worker 崩溃，只做原子操作，zend_try 包裹（Phase 4 补充）。
 */
PHP_RINIT_FUNCTION(beacon)
{
    if (!BEACON_G(enabled) || BEACON_G(shm) == NULL) {
        return SUCCESS;
    }

    /* 心跳槽位注册 + 原子自计数 */
    beacon_shm_worker_register(getpid());

    /* 治理 worker 存活检查（轻量级，每 100 请求一次）
     *
     * kill(pid, 0) 是系统调用但极轻量（不发送信号，只检查进程存在）。
     * 每 100 请求检查一次，避免每个请求都做系统调用。
     * 对标 nginx master 定期 kill(worker, 0) 检测。
     */
    if (++BEACON_G(request_count) % 100 == 0) {
        beacon_governance_ensure_running();
    }

    return SUCCESS;
}

/* RSHUTDOWN：FPM worker per-request 执行
 *
 * 只做原子操作：
 *   1. 心跳槽位写 busy=0
 *   2. pool_busy 原子自减 + pool_total 原子自增
 */
PHP_RSHUTDOWN_FUNCTION(beacon)
{
    if (!BEACON_G(enabled) || BEACON_G(shm) == NULL) {
        return SUCCESS;
    }

    /* 心跳槽位释放 + 原子自计数 */
    beacon_shm_worker_release(getpid());

    return SUCCESS;
}

/* ---- 内部测试函数 arginfo（Phase 2 验证用，Phase 4 PHP API 实现后移除）---- */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_test_health, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_test_set_ready, 0, 0, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, ready, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_test_set_governance_alive, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, ts, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_test_set_pool_counters, 0, 3, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, busy, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, idle, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, total, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_test_store, 0, 2, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, service, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, nodes, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_test_pick, 0, 2, IS_ARRAY, 1)
    ZEND_ARG_TYPE_INFO(0, service, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, strategy, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, exclude, IS_ARRAY, 1)
    ZEND_ARG_TYPE_INFO(0, prefer_healthy, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_test_instances, 0, 1, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, service, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* ---- Phase 3 测试函数 arginfo ---- */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_test_gov_spawn, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_test_gov_is_alive, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_test_gov_pid, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_test_callback_set, 0, 2, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, type, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, callback, IS_CALLABLE, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_test_callback_invoke, 0, 1, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, type, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, ctx, IS_MIXED, 0)
ZEND_END_ARG_INFO()

/* ---- 内部测试函数（Phase 2 验证用，Phase 4 PHP API 实现后移除）----
 *
 * 这些函数暴露 C 层内部接口供 PHP 测试脚本调用。
 * 对标 Swoole 的 swoole_test_* 系列函数。
 * 生产环境不应依赖这些函数——它们仅用于开发期验证。
 */

/* beacon_test_health(): 返回 pool 级健康状态 */
PHP_FUNCTION(beacon_test_health)
{
    beacon_health_t h = beacon_health_calculate();

    array_init(return_value);
    add_assoc_long(return_value, "status", h.status);
    add_assoc_long(return_value, "pool_busy", h.pool_busy);
    add_assoc_long(return_value, "pool_idle", h.pool_idle);
    add_assoc_long(return_value, "pool_total", h.pool_total);
    add_assoc_double(return_value, "saturation", h.saturation);
    add_assoc_bool(return_value, "governance_alive", h.governance_alive);
}

/* beacon_test_set_ready(bool $ready): 设置 pool_ready 标志（模拟预热完成） */
PHP_FUNCTION(beacon_test_set_ready)
{
    zend_bool ready = 1;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "|b", &ready) == FAILURE) {
        return;
    }

    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm) {
        RETURN_FALSE;
    }

    shm->header.pool_ready = ready ? 1 : 0;
    RETURN_TRUE;
}

/* beacon_test_set_governance_alive(int $ts): 设置治理 worker 心跳时间戳 */
PHP_FUNCTION(beacon_test_set_governance_alive)
{
    zend_long ts = 0;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "l", &ts) == FAILURE) {
        return;
    }

    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm) {
        RETURN_FALSE;
    }

    shm->header.governance_alive = (uint64_t)ts;
    RETURN_TRUE;
}

/* beacon_test_set_pool_counters(int $busy, int $idle, int $total): 设置 pool 计数器（测试饱和度） */
PHP_FUNCTION(beacon_test_set_pool_counters)
{
    zend_long busy = 0, idle = 0, total = 0;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "lll", &busy, &idle, &total) == FAILURE) {
        return;
    }

    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm) {
        RETURN_FALSE;
    }

    atomic_store(&shm->header.pool_busy, (unsigned)busy);
    atomic_store(&shm->header.pool_idle, (unsigned)idle);
    atomic_store(&shm->header.pool_total, (unsigned)total);
    RETURN_TRUE;
}

/* beacon_test_store(string $service, array $nodes): 写入测试节点 + commit */
PHP_FUNCTION(beacon_test_store)
{
    char *service;
    size_t service_len;
    zval *nodes_arr;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "sa", &service, &service_len, &nodes_arr) == FAILURE) {
        return;
    }

    beacon_shm_t *shm = BEACON_G(shm);
    if (!shm) {
        RETURN_FALSE;
    }

    uint32_t count = zend_hash_num_elements(Z_ARRVAL_P(nodes_arr));
    if (count > BEACON_MAX_NODES_PER_SVC) {
        count = BEACON_MAX_NODES_PER_SVC;
    }

    beacon_node_t nodes[BEACON_MAX_NODES_PER_SVC];
    memset(nodes, 0, sizeof(nodes));

    uint32_t i = 0;
    zval *entry;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(nodes_arr), entry) {
        if (i >= count) break;
        if (Z_TYPE_P(entry) != IS_ARRAY) continue;

        zval *zid     = zend_hash_str_find(Z_ARRVAL_P(entry), "id",     sizeof("id")-1);
        zval *zhost   = zend_hash_str_find(Z_ARRVAL_P(entry), "host",   sizeof("host")-1);
        zval *zport   = zend_hash_str_find(Z_ARRVAL_P(entry), "port",   sizeof("port")-1);
        zval *zstatus = zend_hash_str_find(Z_ARRVAL_P(entry), "status", sizeof("status")-1);
        zval *zweight = zend_hash_str_find(Z_ARRVAL_P(entry), "weight", sizeof("weight")-1);

        if (zid && Z_TYPE_P(zid) == IS_STRING) {
            strncpy(nodes[i].id, Z_STRVAL_P(zid), BEACON_MAX_NODE_ID_LEN - 1);
        }
        if (zhost && Z_TYPE_P(zhost) == IS_STRING) {
            strncpy(nodes[i].host, Z_STRVAL_P(zhost), BEACON_MAX_HOST_LEN - 1);
        }
        if (zport && Z_TYPE_P(zport) == IS_LONG) {
            nodes[i].port = (uint16_t)Z_LVAL_P(zport);
        }
        if (zstatus && Z_TYPE_P(zstatus) == IS_LONG) {
            nodes[i].status = (uint8_t)Z_LVAL_P(zstatus);
        }
        if (zweight && Z_TYPE_P(zweight) == IS_LONG) {
            nodes[i].weight = (uint16_t)Z_LVAL_P(zweight);
        }
        i++;
    } ZEND_HASH_FOREACH_END();

    if (beacon_shm_store_nodes(service, nodes, i) != 0) {
        RETURN_FALSE;
    }
    if (beacon_shm_commit() != 0) {
        RETURN_FALSE;
    }

    RETURN_TRUE;
}

/* beacon_test_pick(string $service, int $strategy, array $exclude, bool $prefer_healthy): 选取节点 */
PHP_FUNCTION(beacon_test_pick)
{
    char *service;
    size_t service_len;
    zend_long strategy = BEACON_LB_ROUND_ROBIN;
    zval *exclude_arr = NULL;
    zend_bool prefer_healthy = 0;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "sl|ab",
            &service, &service_len, &strategy, &exclude_arr, &prefer_healthy) == FAILURE) {
        return;
    }

    /* 构建 exclude 列表 */
    const char *exclude[BEACON_MAX_NODES_PER_SVC];
    uint32_t exclude_count = 0;
    if (exclude_arr) {
        zval *entry;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(exclude_arr), entry) {
            if (exclude_count >= BEACON_MAX_NODES_PER_SVC) break;
            if (Z_TYPE_P(entry) == IS_STRING) {
                exclude[exclude_count++] = Z_STRVAL_P(entry);
            }
        } ZEND_HASH_FOREACH_END();
    }

    const beacon_node_t *node = beacon_select_pick(
        service, (int)strategy, exclude, exclude_count, prefer_healthy);
    if (!node) {
        RETURN_NULL();
    }

    array_init(return_value);
    add_assoc_string(return_value, "id", node->id);
    add_assoc_string(return_value, "host", node->host);
    add_assoc_long(return_value, "port", node->port);
    add_assoc_long(return_value, "status", node->status);
    add_assoc_long(return_value, "weight", node->weight);
}

/* beacon_test_instances(string $service): 获取全部健康节点 */
PHP_FUNCTION(beacon_test_instances)
{
    char *service;
    size_t service_len;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &service, &service_len) == FAILURE) {
        return;
    }

    beacon_node_t nodes[BEACON_MAX_NODES_PER_SVC];
    uint32_t count = 0;

    if (beacon_select_get_instances(service, nodes, &count) != 0) {
        RETURN_FALSE;
    }

    array_init(return_value);
    for (uint32_t i = 0; i < count; i++) {
        zval node_arr;
        array_init(&node_arr);
        add_assoc_string(&node_arr, "id", nodes[i].id);
        add_assoc_string(&node_arr, "host", nodes[i].host);
        add_assoc_long(&node_arr, "port", nodes[i].port);
        add_assoc_long(&node_arr, "status", nodes[i].status);
        add_assoc_long(&node_arr, "weight", nodes[i].weight);
        add_next_index_zval(return_value, &node_arr);
    }
}

/* ---- Phase 3 测试函数（生命周期层验证用，Phase 4 PHP API 实现后移除）----
 *
 * 这些函数暴露 C 层治理 worker 和回调接口供 PHP 测试脚本调用。
 * 对标 Swoole 的 swoole_test_* 系列函数。
 */

/* beacon_test_gov_spawn(): 手动 spawn 治理 worker，返回 pid */
PHP_FUNCTION(beacon_test_gov_spawn)
{
    pid_t pid = beacon_governance_spawn();
    RETURN_LONG((zend_long)pid);
}

/* beacon_test_gov_is_alive(): 检查治理 worker 是否存活 */
PHP_FUNCTION(beacon_test_gov_is_alive)
{
    RETURN_BOOL(beacon_governance_is_alive());
}

/* beacon_test_gov_pid(): 返回当前治理 worker pid */
PHP_FUNCTION(beacon_test_gov_pid)
{
    RETURN_LONG((zend_long)BEACON_G(governance_pid));
}

/* beacon_test_callback_set(int $type, callable $callback): 设置回调 */
PHP_FUNCTION(beacon_test_callback_set)
{
    zend_long type;
    zval *callback;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "lz", &type, &callback) == FAILURE) {
        return;
    }

    if (type < 0 || type >= BEACON_CB_MAX) {
        php_error_docref(NULL, E_WARNING, "invalid callback type: %lld", (long long)type);
        RETURN_FALSE;
    }

    int result = beacon_callback_set((beacon_callback_type_t)type, callback);
    RETURN_BOOL(result == 0);
}

/* beacon_test_callback_invoke(int $type, ?array $ctx): 调用回调，返回耗时 ms */
PHP_FUNCTION(beacon_test_callback_invoke)
{
    zend_long type;
    zval *ctx = NULL;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "l|z", &type, &ctx) == FAILURE) {
        return;
    }

    if (type < 0 || type >= BEACON_CB_MAX) {
        php_error_docref(NULL, E_WARNING, "invalid callback type: %lld", (long long)type);
        RETURN_LONG(-1);
    }

    zval retval;
    ZVAL_UNDEF(&retval);

    int result = beacon_callback_invoke((beacon_callback_type_t)type, ctx, &retval);

    if (Z_TYPE(retval) != IS_UNDEF) {
        zval_ptr_dtor(&retval);
    }

    RETURN_LONG(result);
}

/* ---- 函数表 ---- */
static const zend_function_entry beacon_functions[] = {
    PHP_FE(beacon_test_health,               arginfo_test_health)
    PHP_FE(beacon_test_set_ready,            arginfo_test_set_ready)
    PHP_FE(beacon_test_set_governance_alive, arginfo_test_set_governance_alive)
    PHP_FE(beacon_test_set_pool_counters,    arginfo_test_set_pool_counters)
    PHP_FE(beacon_test_store,                arginfo_test_store)
    PHP_FE(beacon_test_pick,                 arginfo_test_pick)
    PHP_FE(beacon_test_instances,            arginfo_test_instances)
    PHP_FE(beacon_test_gov_spawn,            arginfo_test_gov_spawn)
    PHP_FE(beacon_test_gov_is_alive,         arginfo_test_gov_is_alive)
    PHP_FE(beacon_test_gov_pid,              arginfo_test_gov_pid)
    PHP_FE(beacon_test_callback_set,         arginfo_test_callback_set)
    PHP_FE(beacon_test_callback_invoke,      arginfo_test_callback_invoke)
    PHP_FE_END
};

/* ---- 模块信息（php -i 输出）---- */
PHP_MINFO_FUNCTION(beacon)
{
    php_info_print_box_start(0);
    php_printf("php-beacon-extension — PHP-FPM state beacon and governance scheduling substrate");
    php_info_print_box_end();

    php_info_print_table_start();
    php_info_print_table_header(2, "beacon support", "enabled");
    php_info_print_table_row(2, "version", PHP_BEACON_VERSION);
    php_info_print_table_row(2, "shm status", BEACON_G(shm) ? "ready" : "degraded (shm unavailable)");
    php_info_print_table_end();

    DISPLAY_INI_ENTRIES();
}

/* ---- 模块入口（对标 Swoole swoole_module_entry / Yar yar_module_entry）---- */
zend_module_entry beacon_module_entry = {
    STANDARD_MODULE_HEADER,
    PHP_BEACON_EXTNAME,             /* 扩展名 */
    beacon_functions,                /* functions（当前为内部测试函数，Phase 4 替换为正式 PHP API） */
    PHP_MINIT(beacon),               /* MINIT */
    PHP_MSHUTDOWN(beacon),           /* MSHUTDOWN */
    PHP_RINIT(beacon),               /* RINIT */
    PHP_RSHUTDOWN(beacon),           /* RSHUTDOWN */
    PHP_MINFO(beacon),               /* MINFO */
    PHP_BEACON_VERSION,              /* version */
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_BEACON
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(beacon)
#endif
