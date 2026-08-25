/* beacon_config.c — INI 配置辅助
 *
 * 职责：提供日志初始化辅助函数。
 * INI 指令表（PHP_INI_BEGIN/END）在 beacon.c 中定义（PHP 扩展惯例）。
 * 预定义常量（OPT_*, LB_*, HEALTH_*）在 beacon_api.c 中注册为类常量
 * （对标 Yar Yar_Client::OPT_* / Redis Redis::OPT_* 的类常量方式）。
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php_beacon.h"
