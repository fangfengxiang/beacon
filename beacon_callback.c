/* beacon_callback.c — C→PHP Callback Invocation
 *
 * 职责：存储 PHP 回调 zval、通过 zend_call_function 调用回调、
 *       异常隔离（zend_try）、计时、慢回调告警。
 *
 * 设计依据：docs/design/spi.md §三 SPI 模式（C 编排 + PHP 实现）
 *           openspec/changes/2026-08-25-lifecycle-layer/design.md §2
 * 业界对标：Swoole swoole_call_function（try-catch 隔离）、
 *           PHP 内部 user_shutdown_function_call（zend_try 隔离）
 *
 * 核心决策：
 *   1. 无 C 层超时（ualarm 已移除）——信号超时非 async-signal-safe，
 *      PHP 层通过 ReactPHP/HTTP client timeout 自管理
 *   2. C 层只做"温和提示"——记录回调耗时，慢则告警，不中断
 *   3. 异常隔离——zend_try 防止回调异常导致治理 worker 崩溃
 *   4. 回调存储——per-type zval，Phase 4 Beacon::setOpt() 设置
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php_beacon.h"

#include <string.h>
#include <time.h>

/* zend_clear_exception 声明在 zend_exceptions.h 中 */
#include <zend_exceptions.h>

/* ---- 慢回调阈值（毫秒）----
 *
 * 500ms 以上视为慢回调，记录 WARN 日志。
 * 对标 Swoole slow_log 500ms、PHP slow_log 500ms（php-fpm request_slowlog_timeout）。
 * 定义为常量消除魔数（500 不自文档化）。
 */
#define BEACON_CALLBACK_SLOW_THRESHOLD_MS  500

/* ---- 回调存储 ----
 *
 * per-type zval 数组，索引为 beacon_callback_type_t 枚举值。
 * Phase 4 Beacon::setOpt(OPT_ON_*, $callback) 调用 beacon_callback_set() 写入。
 * 治理 worker 调用 beacon_callback_invoke() 读取并执行。
 *
 * 使用静态数组而非 HashTable——回调类型是有限集（5 种），数组 O(1) 访问。
 * 对标 PHP user_shutdown_function_list（静态数组）。
 */
static zval callback_storage[BEACON_CB_MAX];

/* 初始化回调存储（MINIT 调用） */
void beacon_callback_init(void)
{
    memset(callback_storage, 0, sizeof(callback_storage));
}

/* 清理回调存储（MSHUTDOWN 调用） */
void beacon_callback_cleanup(void)
{
    for (int i = 0; i < BEACON_CB_MAX; i++) {
        if (Z_TYPE(callback_storage[i]) != IS_UNDEF) {
            zval_ptr_dtor(&callback_storage[i]);
            ZVAL_UNDEF(&callback_storage[i]);
        }
    }
}

/* ---- 设置回调 ----
 *
 * 将 PHP 回调 zval 存储到指定 type 的槽位。
 * 如果已有回调，先释放旧 zval 再存新的。
 *
 * 返回 0 成功 / -1 失败（type 无效或 callback 不是 callable）。
 *
 * 对标 Swoole swoole_event_handler_set、PHP register_shutdown_function。
 */
int beacon_callback_set(beacon_callback_type_t type, zval *callback)
{
    if (type < 0 || type >= BEACON_CB_MAX) {
        return -1;
    }

    /* 验证 callback 是 callable */
    if (!callback || Z_TYPE_P(callback) == IS_NULL) {
        /* NULL 表示清除回调 */
        if (Z_TYPE(callback_storage[type]) != IS_UNDEF) {
            zval_ptr_dtor(&callback_storage[type]);
            ZVAL_UNDEF(&callback_storage[type]);
        }
        return 0;
    }

    /* 检查是否为 callable */
    char *error_str = NULL;
    zend_fcall_info_cache fci_cache;

    if (!zend_is_callable_ex(callback, NULL, 0, NULL, &fci_cache, &error_str)) {
        if (error_str) {
            beacon_log(BEACON_LOG_WARN, "callback set: not callable: %s", error_str);
            efree(error_str);
        }
        return -1;
    }
    if (error_str) {
        efree(error_str);
    }

    /* 释放旧回调 */
    if (Z_TYPE(callback_storage[type]) != IS_UNDEF) {
        zval_ptr_dtor(&callback_storage[type]);
    }

    /* 存储新回调（复制 zval，增加引用计数） */
    ZVAL_COPY(&callback_storage[type], callback);

    return 0;
}

/* ---- 获取回调 ----
 *
 * 返回指定 type 的回调 zval 指针，或 NULL（未设置）。
 */
zval *beacon_callback_get(beacon_callback_type_t type)
{
    if (type < 0 || type >= BEACON_CB_MAX) {
        return NULL;
    }

    if (Z_TYPE(callback_storage[type]) == IS_UNDEF ||
        Z_TYPE(callback_storage[type]) == IS_NULL) {
        return NULL;
    }

    return &callback_storage[type];
}

/* ---- 调用回调 ----
 *
 * 通过 zend_call_function 调用 PHP 回调，包裹 zend_try 隔离异常。
 * 测量回调耗时，超过 500ms 记录 WARN。
 *
 * 参数：
 *   type    — 回调类型（BEACON_CB_ON_*）
 *   ctx     — 上下文参数 zval（传给回调的第一个参数，可为 NULL）
 *   retval  — 返回值 zval（调用方分配，可为 NULL 表示忽略返回值）
 *
 * 返回 0 成功 / -1 失败（回调未设置或异常）。
 *
 * 对标 Swoole swoole_call_function（try-catch + timing）、
 *       PHP user_shutdown_function_call（zend_try 隔离）。
 *
 * 设计决策：
 *   - 无 C 层超时（ualarm 移除）——信号超时非 async-signal-safe
 *   - C 层只做"温和提示"——记录耗时，慢则告警，不中断
 *   - 异常隔离——zend_try 防止回调异常导致治理 worker 崩溃
 */
int beacon_callback_invoke(beacon_callback_type_t type, zval *ctx, zval *retval)
{
    zval *callback = beacon_callback_get(type);
    if (!callback) {
        /* 回调未设置——不是错误，只是没有注册回调 */
        return -1;
    }

    /* 计时开始 */
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    /* 准备调用参数 */
    zval ret_val;
    ZVAL_UNDEF(&ret_val);

    zval params[1];
    int param_count = 0;
    if (ctx) {
        ZVAL_COPY_VALUE(&params[0], ctx);
        param_count = 1;
    }

    /* 初始化 fcall_info */
    zend_fcall_info fci;
    zend_fcall_info_cache fci_cache;
    char *error_str = NULL;

    if (!zend_is_callable_ex(callback, NULL, 0, NULL, &fci_cache, &error_str)) {
        if (error_str) {
            beacon_log(BEACON_LOG_WARN, "callback invoke: not callable: %s", error_str);
            efree(error_str);
        }
        return -1;
    }
    if (error_str) {
        efree(error_str);
    }

    /* 必须清零 fci，否则未初始化字段（如 named_params）会导致 segfault */
    memset(&fci, 0, sizeof(fci));
    fci.size = sizeof(fci);
    fci.object = NULL;
    ZVAL_COPY_VALUE(&fci.function_name, callback);
    fci.params = (param_count > 0) ? params : NULL;
    fci.param_count = param_count;
    fci.retval = &ret_val;

    /* zend_try 隔离异常——防止回调异常导致治理 worker 崩溃 */
    bool success = true;

    zend_try {
        int call_result = zend_call_function(&fci, &fci_cache);
        if (call_result != SUCCESS) {
            success = false;
        }

        /* 如果有异常未捕获，标记失败 */
        if (EG(exception) != NULL) {
            success = false;
            /* 清除异常，防止传播 */
            zend_clear_exception();
        }
    } zend_catch {
        success = false;
        beacon_log(BEACON_LOG_ERROR, "callback invoke: zend_try caught fatal error (type=%d)",
                   (int)type);
    } zend_end_try();

    /* 计时结束 */
    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    long elapsed_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000 +
                      (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000;

    /* 慢回调告警 */
    if (elapsed_ms > BEACON_CALLBACK_SLOW_THRESHOLD_MS) {
        beacon_log(BEACON_LOG_WARN, "slow callback: type=%d took %ldms (threshold=%dms)",
                   (int)type, elapsed_ms, BEACON_CALLBACK_SLOW_THRESHOLD_MS);
    }

    /* 处理返回值 */
    if (retval) {
        if (success && Z_TYPE(ret_val) != IS_UNDEF) {
            ZVAL_COPY_VALUE(retval, &ret_val);
        } else {
            ZVAL_NULL(retval);
        }
    } else {
        /* 调用方不关心返回值，释放 */
        if (Z_TYPE(ret_val) != IS_UNDEF) {
            zval_ptr_dtor(&ret_val);
        }
    }

    /* 释放参数引用 */
    if (ctx) {
        /* params[0] 是 ctx 的 copy，不需要单独释放 */
    }

    return success ? 0 : -1;
}
