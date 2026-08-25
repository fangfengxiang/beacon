/* php_beacon.h — php-beacon-extension module header
 *
 * PHP-FPM 状态信标与治理调度基底扩展
 *
 * 设计依据：docs/design/ (13 files)
 * 核心约束：扩展任何异常都不应导致 FPM 无法服务请求
 *
 * 业界对标：Swoole (php_swoole.h) / Yar (php_yar.h) 的模块头文件组织
 */

#ifndef PHP_BEACON_H
#define PHP_BEACON_H

#define PHP_BEACON_VERSION "0.1.0-dev"
#define PHP_BEACON_EXTNAME "beacon"

/* C 标准头 */
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <sys/types.h>  /* pid_t */

/* Zend 头文件 */
#include "zend.h"
#include "zend_globals.h"
#include "php.h"
#include "ext/standard/info.h"

/* shm 结构体定义（infra-shm capability 共享） */
#include "beacon_shm.h"
/* 日志工具 */
#include "beacon_log.h"

extern zend_module_entry beacon_module_entry;
#define phpext_beacon_ptr &beacon_module_entry

/* ---- 全局结构体（对标 Yar YAR_G / Swoole SWOOLE_G）----
 *
 * 承载所有 INI 解析结果与运行时全局状态。
 * 线程安全构建（ZTS）由 ZEND_BEGIN_MODULE_GLOBALS 宏自动处理 per-thread 存储。
 */
ZEND_BEGIN_MODULE_GLOBALS(beacon)
    /* INI 解析结果 */
    zend_bool   enabled;              /* beacon.enabled */
    char       *service_name;          /* beacon.service_name */
    char       *advertise_host;        /* beacon.advertise_host */
    char       *advertise_host_env;    /* beacon.advertise_host_env */
    char       *advertise_port;        /* beacon.advertise_port */
    char       *registry_endpoint;     /* beacon.registry_endpoint */
    char       *governance_bin;         /* beacon.governance_bin */
    char       *governance_script;      /* beacon.governance_script */
    zend_long  keepalive_interval;     /* beacon.keepalive_interval */
    zend_long  pull_interval;           /* beacon.pull_interval */
    zend_long  heartbeat_ttl;           /* beacon.heartbeat_ttl */
    zend_long  health_dead_threshold;  /* beacon.health_dead_threshold */
    char       *lb_strategy;           /* beacon.lb_strategy */
    zend_long  shm_key;                /* beacon.shm_key */
    char       *log_file;              /* beacon.log_file */
    char       *log_level;             /* beacon.log_level */

    /* 运行时状态 */
    int         shm_id;                /* sysv shm shmid，-1 = 未初始化 */
    beacon_shm_t *shm;                 /* shmat 返回的指针，NULL = 不可用 */
    zend_bool   is_shm_owner;          /* 本进程是否为 shm 创建者（MSHUTDOWN 时决定是否 IPC_RMID） */
    pid_t       governance_pid;        /* 治理 worker PID（master 视角） */
    int         governance_restart_count; /* 治理 worker 重启计数（master 视角） */
    uint32_t    request_count;         /* 请求计数（RINIT 自增，用于治理 worker 周期检查） */
    int         runtime_lb_strategy;  /* 运行时 LB 策略覆盖（setOpt 设置，0 = 未覆盖用 INI 默认） */
ZEND_END_MODULE_GLOBALS(beacon)
ZEND_EXTERN_MODULE_GLOBALS(beacon)

/* 全局访问宏（对标 YAR_G / SWOOLE_G） */
#ifdef ZTS
#define BEACON_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(beacon, v)
#else
#define BEACON_G(v) (beacon_globals.v)
#endif

/* ---- 共享常量（供 domain service 使用）----
 *
 * 这些数值常量在 beacon_api.c 中用 zend_declare_class_constant_long/string 注册为
 * Beacon 类常量（Beacon::LB_* / Beacon::OPT_* / Beacon::HEALTH_*），同时在 C 层供
 * domain service 的 switch-case 使用。
 */

/* LB 策略枚举（Beacon::LB_*，用于 OPT_LB_STRATEGY 的值） */
#define BEACON_LB_ROUND_ROBIN  1
#define BEACON_LB_RANDOM       2
#define BEACON_LB_WEIGHTED     3

/* 选项 key 常量（Beacon::OPT_*，用于 setOpt() 的 key） */
#define BEACON_OPT_ON_REGISTER              1
#define BEACON_OPT_ON_KEEPALIVE             2
#define BEACON_OPT_ON_DISCOVER              3
#define BEACON_OPT_ON_DEREGISTER            4
#define BEACON_OPT_ON_WATCH                 5
#define BEACON_OPT_LB_STRATEGY              6
#define BEACON_OPT_KEEPALIVE_INTERVAL       7
#define BEACON_OPT_PULL_INTERVAL            8
#define BEACON_OPT_HEARTBEAT_TTL            9
#define BEACON_OPT_HEALTH_DEAD_THRESHOLD   10
#define BEACON_OPT_EXCLUDE                  11
#define BEACON_OPT_PREFER_HEALTHY           12

/* 健康状态数值码（C 层内部使用，对应 PHP userland 的字符串常量）
 *
 * PHP userland 看到：Beacon::HEALTH_OK = "ok"（字符串）
 * C 层内部用：BEACON_HEALTH_STATUS_OK = 1（数值）
 * PHP API 层（Phase 4）负责数值→字符串映射。
 */
#define BEACON_HEALTH_STATUS_NOT_READY  0
#define BEACON_HEALTH_STATUS_OK         1
#define BEACON_HEALTH_STATUS_DEGRADED   2
#define BEACON_HEALTH_STATUS_DEAD       3

/* 节点状态数值码（beacon_node_t.status 字段，与 shm-design.md §二 一致） */
#define BEACON_NODE_STATUS_OK           0
#define BEACON_NODE_STATUS_DEGRADED     1
#define BEACON_NODE_STATUS_DEAD         2
#define BEACON_NODE_STATUS_NOT_READY    3

/* ---- 回调类型枚举（C→PHP 回调调用，Phase 3）----
 *
 * 对标 Swoole Event::PHP_CALLBACK、PHP register_shutdown_function。
 * 与 BEACON_OPT_ON_* 常量值一致（1-5），便于 Beacon::setOpt() 直接映射。
 */
typedef enum {
    BEACON_CB_ON_REGISTER   = 1,
    BEACON_CB_ON_KEEPALIVE  = 2,
    BEACON_CB_ON_DISCOVER   = 3,
    BEACON_CB_ON_DEREGISTER = 4,
    BEACON_CB_ON_WATCH      = 5,
    BEACON_CB_MAX           = 6,  /* 上界，用于数组大小 */
} beacon_callback_type_t;

/* ---- 基础设施层函数声明 ---- */

/* beacon_config.c — INI 解析与日志初始化辅助 */
void beacon_log_init_globals(void);

/* beacon_shm.c — sysv shm 双缓冲 */
int  beacon_shm_init(void);            /* MINIT 调用，返回 0 成功 / -1 失败 */
void beacon_shm_destroy(void);         /* MSHUTDOWN 调用 */
int  beacon_shm_worker_register(pid_t pid);   /* RINIT 调用 */
int  beacon_shm_worker_release(pid_t pid);    /* RSHUTDOWN 调用 */

/* ---- 领域服务层函数声明 ---- */

/* beacon_service_health.c — pool 级健康状态计算
 *
 * 返回 beacon_health_t，含 status（数值码）+ metrics（busy/idle/total/
 * saturation/governance_alive）。治理 worker keepalive tick 中调。
 */
typedef struct {
    uint8_t  status;          /* BEACON_HEALTH_STATUS_* */
    uint32_t pool_busy;       /* 当前 busy worker 数 */
    uint32_t pool_idle;       /* 当前 idle worker 数 */
    uint32_t pool_total;      /* 累计处理请求数 */
    double   saturation;      /* 饱和度 0.0-1.0 */
    bool     governance_alive;/* 治理 worker 是否存活 */
} beacon_health_t;

beacon_health_t beacon_health_calculate(void);

/* beacon_service_select.c — 节点选取与负载均衡
 *
 * beacon_select_pick: 选取一个节点（Beacon::pick() 调）
 *   返回指向 shm 的指针（零拷贝）或 NULL（无可用节点）
 * beacon_select_get_instances: 获取全部健康节点（Beacon::getInstances() 调）
 *   拷贝到调用方数组，返回 0 成功 / -1 失败
 */
const beacon_node_t *beacon_select_pick(
    const char *service,
    int strategy,
    const char *const *exclude,
    uint32_t exclude_count,
    bool prefer_healthy);

int beacon_select_get_instances(
    const char *service,
    beacon_node_t *out_nodes,
    uint32_t *out_count);

/* ---- 生命周期层函数声明（Phase 3）---- */

/* beacon_governance_worker.c — 治理 worker 生命周期管理
 *
 * spawn: fork + close_fds + setsid + prctl + execl，返回 pid 或 -1
 * is_alive: kill(pid, 0) 存活检测
 * ensure_running: 检查存活 → 死则重启（重试上限 5）→ 降级模式
 * shutdown: SIGTERM + waitpid(2s) + SIGKILL fallback
 * is_governance_worker: 检查 BEACON_GOVERNANCE_WORKER 环境变量（防无限 fork）
 */
bool  beacon_is_governance_worker(void);
pid_t beacon_governance_spawn(void);
bool  beacon_governance_is_alive(void);
int   beacon_governance_ensure_running(void);
void  beacon_governance_shutdown(void);

/* beacon_callback.c — C→PHP 回调调用
 *
 * init: 初始化回调存储（MINIT 调用）
 * cleanup: 清理回调存储（MSHUTDOWN 调用）
 * set: 存储回调 zval（Phase 4 Beacon::setOpt 调用）
 * get: 获取回调 zval（按 type）
 * invoke: zend_call_function + zend_try + timing + warn
 */
void beacon_callback_init(void);
void beacon_callback_cleanup(void);
int  beacon_callback_set(beacon_callback_type_t type, zval *callback);
zval *beacon_callback_get(beacon_callback_type_t type);
int  beacon_callback_invoke(beacon_callback_type_t type, zval *ctx, zval *retval);

/* ---- PHP API 层函数声明（Phase 4）---- */

/* beacon_api.c — Beacon 类注册与方法实现
 *
 * register_beacon_class: MINIT 调用，注册 Beacon 类（静态方法）
 * 对标 Swoole swoole_register_classes / Yar yar_register_classes
 */
void beacon_register_beacon_class(int module_number);

/* beacon_api_governance.c — Beacon\Governance 类注册与方法实现
 *
 * register_governance_class: MINIT 调用，注册 Beacon\Governance 类（CLI SAPI 专用）
 */
void beacon_register_governance_class(int module_number);

/* 便捷聚合：注册所有 PHP 类（beacon.c MINIT 调用） */
static inline void beacon_register_classes(int module_number)
{
    beacon_register_beacon_class(module_number);
    beacon_register_governance_class(module_number);
}

#endif /* PHP_BEACON_H */
