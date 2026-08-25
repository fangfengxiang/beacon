/* beacon_log.h — php-beacon-extension logging utility
 *
 * 日志工具：治理 worker（CLI SAPI，无请求上下文）用自定义文件日志，
 * FPM worker 请求路径用 php_error_docref。
 *
 * 业界对标：Swoole swLog（文件 + 级别 + 轮转）
 */

#ifndef BEACON_LOG_H
#define BEACON_LOG_H

#include <stdarg.h>

/* 日志级别（对标 syslog priority，数值越小越严重） */
#define BEACON_LOG_DEBUG  1
#define BEACON_LOG_INFO   2
#define BEACON_LOG_WARN   3
#define BEACON_LOG_ERROR  4

/* 日志宏：编译期过滤 DEBUG（生产构建零开销）
 *
 * 用法：beacon_log(BEACON_LOG_WARN, "busy counter drift: shm=%d", shm_busy);
 */
void beacon_log_impl(int level, const char *file, int line, const char *fmt, ...);

#define beacon_log(level, ...) \
    do { \
        if ((level) >= BEACON_LOG_INFO) { \
            beacon_log_impl((level), __FILE__, __LINE__, __VA_ARGS__); \
        } \
    } while (0)

#ifdef BEACON_DEBUG
#define beacon_log_debug(...) beacon_log_impl(BEACON_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#else
#define beacon_log_debug(...) ((void)0)
#endif

/* 从 INI 初始化日志全局状态（MINIT 调用） */
void beacon_log_init_globals(void);

#endif /* BEACON_LOG_H */
