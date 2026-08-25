/* beacon_log.c — php-beacon-extension logging utility implementation
 *
 * 治理 worker（CLI SAPI）写文件或 stderr，由 beacon.log_file / beacon.log_level INI 驱动。
 * FPM worker 请求路径错误用 php_error_docref（PHP 标准）。
 *
 * 业界对标：Swoole swLog（文件 + 级别 + 时间戳）
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php_beacon.h"

#include <string.h>
#include <time.h>
#include <stdio.h>
#include <errno.h>

/* 日志级别名称（对标 syslog） */
static const char *level_names[] = {
    [BEACON_LOG_DEBUG] = "DEBUG",
    [BEACON_LOG_INFO]  = "INFO",
    [BEACON_LOG_WARN]  = "WARN",
    [BEACON_LOG_ERROR] = "ERROR"
};

/* 解析 INI log_level 字符串为数值（默认 WARN） */
static int beacon_log_parse_level(const char *level_str)
{
    if (!level_str) return BEACON_LOG_WARN;
    if (strcasecmp(level_str, "debug") == 0) return BEACON_LOG_DEBUG;
    if (strcasecmp(level_str, "info")  == 0) return BEACON_LOG_INFO;
    if (strcasecmp(level_str, "warn")  == 0) return BEACON_LOG_WARN;
    if (strcasecmp(level_str, "error") == 0) return BEACON_LOG_ERROR;
    return BEACON_LOG_WARN;
}

/* 从 INI 初始化日志全局状态 */
void beacon_log_init_globals(void)
{
    /* 日志级别与文件路径在 beacon_log_impl 中实时读 BEACON_G，
     * 无需缓存——INI OnUpdate 已更新全局结构体 */
}

/* 日志实现：写文件或 stderr
 *
 * 格式：[2026-08-25 12:34:56.789] [WARN] [beacon_shm.c:123] message
 * 对标 Swoole swLog_put 格式
 */
void beacon_log_impl(int level, const char *file, int line, const char *fmt, ...)
{
    /* 级别过滤 */
    int configured_level = beacon_log_parse_level(BEACON_G(log_level));
    if (level < configured_level) {
        return;
    }

    /* 时间戳 */
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);

    /* 提取文件名（去掉路径，只留 basename） */
    const char *basename = strrchr(file, '/');
    basename = basename ? basename + 1 : file;

    /* 格式化消息 */
    char msg_buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    const char *level_name = (level >= BEACON_LOG_DEBUG && level <= BEACON_LOG_ERROR) ? level_names[level] : "UNKNOWN";

    /* 写文件或 stderr */
    const char *log_file = BEACON_G(log_file);
    FILE *fp = NULL;

    if (log_file && log_file[0] != '\0') {
        fp = fopen(log_file, "a");
        if (!fp) {
            /* 文件打开失败，降级写 stderr */
            fprintf(stderr, "[%s] [%s] [%s:%d] %s (log_file open failed: %s)\n",
                    time_str, level_name, basename, line, msg_buf, strerror(errno));
            return;
        }
    } else {
        fp = stderr;
    }

    fprintf(fp, "[%s] [%s] [%s:%d] %s\n", time_str, level_name, basename, line, msg_buf);

    if (fp != stderr) {
        fclose(fp);
    }
}
