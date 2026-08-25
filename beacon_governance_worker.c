/* beacon_governance_worker.c — Governance Worker Lifecycle
 *
 * 职责：spawn（fork+exec）、monitor（kill(pid,0)）、restart（retry limit 5）、
 *       shutdown（SIGTERM + waitpid + SIGKILL fallback）。
 *
 * 设计依据：docs/design/governance-worker.md §二 spawn 机制、§五 崩溃与重启
 *           openspec/changes/2026-08-25-lifecycle-layer/design.md
 * 业界对标：Swoole (swoole_process fork+exec)、systemd (Restart=on-failure)、
 *           nginx (master-worker process management)
 *
 * 核心决策：
 *   1. fork + exec（非纯 fork）——PHP VM 单例，fork 后 VM 状态污染，必须 exec 重置
 *   2. prctl(PR_SET_PDEATHSIG) Linux / getppid() macOS——父进程死亡检测双保险
 *   3. 环境变量 BEACON_GOVERNANCE_WORKER=1 防止子进程 MINIT 再次 spawn（无限 fork 循环）
 *   4. 重试上限 5 次——防止治理 worker 反复崩溃导致 master 耗尽资源
 *   5. 降级模式——重试耗尽后停止重启，FPM 仍可服务请求（用 shm 缓存）
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php_beacon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <time.h>

#include <dirent.h>  /* fd 目录遍历（Linux /proc/self/fd，macOS/BSD /dev/fd） */

#ifdef HAVE_PRCTL
#include <sys/prctl.h>
#endif

/* ---- 常量 ----
 *
 * 重试上限：5 次。对标 systemd RestartSec + StartLimitBurst。
 * 超过 5 次进入降级模式，停止重启。
 * 定义为常量消除魔数（5 不自文档化）。
 */
#define BEACON_GOVERNANCE_MAX_RESTARTS  5

/* waitpid 超时 2 秒（微秒），超时后 SIGKILL */
#define BEACON_WAITPID_TIMEOUT_USEC     2000000

/* 环境变量名：子进程 guard，防止无限 fork 循环 */
#define BEACON_GOVERNANCE_WORKER_ENV    "BEACON_GOVERNANCE_WORKER"

/* exec argv 容量上限（php + 15 个 -d 参数对 + script + NULL = 33，留余量） */
#define BEACON_EXEC_MAX_ARGS            40

/* 单条 "-d key=value" 格式化缓冲区数量与长度 */
#define BEACON_EXEC_INI_BUFS            16
#define BEACON_EXEC_INI_LEN             256

/* ---- 进程内重启计数器 ----
 *
 * per-FPM-master，不跨进程共享（master 是唯一 spawn 者）。
 * 静态变量足够——master 进程内单例。
 */
static int restart_count = 0;

/* ---- 判断当前进程是否为治理 worker ----
 *
 * 检查 BEACON_GOVERNANCE_WORKER 环境变量是否为 "1"。
 * 父进程在 exec 前设置此变量，子进程 MINIT 检查以防止无限 fork 循环。
 *
 * 对标 systemd ConditionEnvironment、nginx env guard。
 */
bool beacon_is_governance_worker(void)
{
    const char *env = getenv(BEACON_GOVERNANCE_WORKER_ENV);
    return env != NULL && env[0] == '1' && env[1] == '\0';
}

/* ---- 关闭所有非标准 fd ----
 *
 * 平台路径：
 *   Linux:     /proc/self/fd（procfs）
 *   macOS/BSD: /dev/fd（fdesc 默认挂载；macOS 无 closefrom()）
 *   兜底:      sysconf(_SC_OPEN_MAX) 盲扫（目录不可用或未知平台）
 *
 * 防止子进程继承 FPM 的 listen socket、数据库连接等。
 * 对标 libuv uv__process_close_fds（Linux /proc/self/fd + 其他平台 sysconf 循环）、
 *      OpenSSH closefrom fallback 循环。
 */
static void close_all_fds_except_std(void)
{
#if defined(__linux__)
    const char *fd_dir_path = "/proc/self/fd";
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__sun)
    const char *fd_dir_path = "/dev/fd";
#else
    const char *fd_dir_path = NULL;
#endif

    if (fd_dir_path) {
        DIR *dir = opendir(fd_dir_path);
        if (dir) {
            int dir_fd = dirfd(dir);
            struct dirent *entry;
            /* 先收集 fd 再关闭，避免遍历时 closedir 改变目录状态。
             * 1024 上限：FPM master 实际 fd 数远小于此，超出部分由 FD_CLOEXEC 兜底 */
            int fds[1024];
            int count = 0;

            while ((entry = readdir(dir)) != NULL && count < 1024) {
                int fd = atoi(entry->d_name);
                if (fd > 2 && fd != dir_fd) {
                    fds[count++] = fd;
                }
            }
            closedir(dir);

            for (int i = 0; i < count; i++) {
                close(fds[i]);
            }
            return;
        }
        /* 目录不可用（如精简环境未挂载 fdesc）→ 落入盲扫兜底 */
    }

    /* 兜底：POSIX sysconf 盲扫（替代 macOS 已 deprecated 的 getdtablesize）。
     * close(EBADF) 开销纳秒级，盲扫无害 */
    long max_fd = sysconf(_SC_OPEN_MAX);
    for (long fd = 3; fd < max_fd; fd++) {
        close((int)fd);
    }
}

/* ---- 获取 PHP CLI 二进制路径 ----
 *
 * 优先级：INI beacon.governance_bin > 编译时宏 BEACON_PHP_BIN。
 * 对标 systemd ExecStart= 路径解析、nginx SBIN_PATH。
 */
static const char *beacon_get_php_bin(void)
{
    if (BEACON_G(governance_bin) && BEACON_G(governance_bin)[0] != '\0') {
        return BEACON_G(governance_bin);
    }
    return BEACON_PHP_BIN;
}

/* ---- 获取治理脚本路径 ----
 *
 * 优先级：INI beacon.governance_script > 编译时宏 BEACON_DATA_DIR "/governance.php"。
 */
static const char *beacon_get_governance_script(void)
{
    if (BEACON_G(governance_script) && BEACON_G(governance_script)[0] != '\0') {
        return BEACON_G(governance_script);
    }
    return BEACON_DATA_DIR "/governance.php";
}

/* ---- 构造治理 worker exec argv ----
 *
 * 子进程是全新 PHP CLI 进程：不继承父进程 -d 参数，系统 php.ini 未必加载扩展。
 * 显式构造 -d 参数：加载扩展 + 传递治理相关 INI。
 * shm_key 非 0 时必须传——否则子进程 ftok 生成的 key 与父进程显式配置不同，
 * attach 不到同一段 shm。
 * 对标 systemd Environment= 显式环境构造（优于隐式继承）。
 */
static void beacon_build_governance_argv(char **argv, char (*ini_buf)[BEACON_EXEC_INI_LEN],
                                         const char *script)
{
    int argc = 0;
    int nini = 0;

    argv[argc++] = "php";

    /* 加载扩展（子进程与父进程同一 PHP 安装，extension-dir 相同；
     * 系统 php.ini 已加载时重复加载仅 warning，无害） */
    argv[argc++] = "-d";
    argv[argc++] = "extension=" PHP_BEACON_EXTNAME ".so";

    /* enabled 必须显式传 1：子进程 MINIT 检查 enabled 才初始化 shm */
    argv[argc++] = "-d";
    argv[argc++] = "beacon.enabled=1";

    /* 字符串 INI：非空才传（减少参数噪音） */
    const struct { const char *key; const char *val; } str_inis[] = {
        { "beacon.service_name",       BEACON_G(service_name) },
        { "beacon.advertise_host",     BEACON_G(advertise_host) },
        { "beacon.advertise_host_env", BEACON_G(advertise_host_env) },
        { "beacon.advertise_port",     BEACON_G(advertise_port) },
        { "beacon.registry_endpoint",  BEACON_G(registry_endpoint) },
        { "beacon.lb_strategy",        BEACON_G(lb_strategy) },
        { "beacon.log_file",           BEACON_G(log_file) },
        { "beacon.log_level",          BEACON_G(log_level) },
    };
    for (size_t i = 0; i < sizeof(str_inis) / sizeof(str_inis[0]); i++) {
        if (str_inis[i].val && str_inis[i].val[0] != '\0') {
            snprintf(ini_buf[nini], BEACON_EXEC_INI_LEN, "%s=%s",
                     str_inis[i].key, str_inis[i].val);
            argv[argc++] = "-d";
            argv[argc++] = ini_buf[nini++];
        }
    }

    /* 数值 INI：治理循环参数总是传（父进程值即真相） */
    const struct { const char *key; zend_long val; } num_inis[] = {
        { "beacon.keepalive_interval",     BEACON_G(keepalive_interval) },
        { "beacon.pull_interval",          BEACON_G(pull_interval) },
        { "beacon.heartbeat_ttl",          BEACON_G(heartbeat_ttl) },
        { "beacon.health_dead_threshold",  BEACON_G(health_dead_threshold) },
    };
    for (size_t i = 0; i < sizeof(num_inis) / sizeof(num_inis[0]); i++) {
        snprintf(ini_buf[nini], BEACON_EXEC_INI_LEN, "%s=" ZEND_LONG_FMT,
                 num_inis[i].key, num_inis[i].val);
        argv[argc++] = "-d";
        argv[argc++] = ini_buf[nini++];
    }

    /* shm_key 非 0 必须传：保证子进程 attach 到父进程同一段 shm */
    if (BEACON_G(shm_key) != 0) {
        snprintf(ini_buf[nini], BEACON_EXEC_INI_LEN, "beacon.shm_key=" ZEND_LONG_FMT,
                 BEACON_G(shm_key));
        argv[argc++] = "-d";
        argv[argc++] = ini_buf[nini++];
    }

    argv[argc++] = (char *)script;
    argv[argc] = NULL;
}

/* ---- Spawn 治理 worker ----
 *
 * fork + close_fds + setsid + prctl(PR_SET_PDEATHSIG) + execv。
 *
 * 返回 pid_t：
 *   > 0  — 成功，子进程 pid
 *   -1   — fork 或 exec 失败
 *
 * 对标 Swoole swoole_process_create、systemd fork+exec。
 * 设计依据：docs/design/governance-worker.md §2.1 启动流程
 */
pid_t beacon_governance_spawn(void)
{
    /* 在 fork 前获取路径（fork 后内存状态一致） */
    const char *php_bin = beacon_get_php_bin();
    const char *script = beacon_get_governance_script();

    /* 检查路径有效性 */
    if (!php_bin || php_bin[0] == '\0') {
        beacon_log(BEACON_LOG_ERROR, "governance spawn: php binary path is empty");
        return -1;
    }
    if (!script || script[0] == '\0') {
        beacon_log(BEACON_LOG_ERROR, "governance spawn: governance script path is empty");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        beacon_log(BEACON_LOG_ERROR, "governance spawn: fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* ---- 子进程 ---- */

        /* 关闭所有非标准 fd（防止继承 FPM 的 listen socket 等） */
        close_all_fds_except_std();

        /* 脱离控制终端（防止 FPM 收到 SIGHUP 时传导给治理 worker） */
        setsid();

        /* Linux: 设置父进程死亡信号——内核在父进程退出时自动发 SIGTERM */
#ifdef HAVE_PRCTL
        prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif

        /* 设置环境变量 guard（子进程 MINIT 检查此变量，防止无限 fork 循环） */
        setenv(BEACON_GOVERNANCE_WORKER_ENV, "1", 1);

        /* 构造 argv 并 exec：替换地址空间，启动全新 PHP VM */
        char *argv[BEACON_EXEC_MAX_ARGS];
        char ini_buf[BEACON_EXEC_INI_BUFS][BEACON_EXEC_INI_LEN];
        beacon_build_governance_argv(argv, ini_buf, script);
        execv(php_bin, argv);

        /* exec 失败只有走到这里 */
        fprintf(stderr, "[beacon] governance spawn: execv failed: %s (bin=%s, script=%s)\n",
                strerror(errno), php_bin, script);
        _exit(127);
    }

    /* ---- 父进程（FPM master）---- */

    /* 记录 pid 到全局状态 */
    BEACON_G(governance_pid) = pid;

    /* 记录 pid 到 shm header（供 FPM worker 读） */
    beacon_shm_t *shm = BEACON_G(shm);
    if (shm) {
        shm->header.governance_pid = pid;
        /* 记录启动时间戳作为初始心跳 */
        shm->header.governance_alive = (uint64_t)time(NULL);
    }

    beacon_log(BEACON_LOG_INFO, "governance worker spawned (pid=%d, bin=%s, script=%s)",
               (int)pid, php_bin, script);

    return pid;
}

/* ---- 检查治理 worker 是否存活 ----
 *
 * kill(pid, 0) == 0 → 存活
 * kill(pid, 0) == -1 && errno == ESRCH → 进程不存在
 *
 * 对标 systemd MainPID liveness check、nginx kill(worker_pid, 0)。
 *
 * 返回 true 存活 / false 不存活。
 */
bool beacon_governance_is_alive(void)
{
    pid_t pid = BEACON_G(governance_pid);

    /* pid <= 0 表示从未 spawn 或已清理 */
    if (pid <= 0) {
        return false;
    }

    /* kill(pid, 0) 不发信号，只检查进程是否存在 */
    if (kill(pid, 0) == 0) {
        return true;
    }

    /* kill 失败——ESRCH 表示进程不存在，其他错误也视为不存活 */
    return false;
}

/* ---- 确保治理 worker 正在运行（必要时重启）----
 *
 * 1. 如果存活 → 返回 0（no-op）
 * 2. 如果不存活且重启次数 < 5 → spawn 新 worker，返回新 pid
 * 3. 如果重启次数 >= 5 → 进入降级模式，返回 -1
 *
 * 对标 systemd StartLimitBurst + StartLimitAction=none。
 * 设计依据：docs/design/governance-worker.md §五 崩溃与重启
 *
 * 返回：
 *   > 0  — 成功 spawn，新 pid
 *    0  — worker 已在运行，no-op
 *   -1  — 降级模式（重试耗尽）
 */
int beacon_governance_ensure_running(void)
{
    /* 存活 → no-op */
    if (beacon_governance_is_alive()) {
        return 0;
    }

    /* 不存活 → 检查重启次数 */
    if (restart_count >= BEACON_GOVERNANCE_MAX_RESTARTS) {
        beacon_log(BEACON_LOG_ERROR,
                   "governance worker restart limit reached (%d), entering degraded mode",
                   restart_count);
        return -1;
    }

    /* 尝试重启 */
    restart_count++;
    beacon_log(BEACON_LOG_WARN,
               "governance worker not alive, restarting (attempt %d/%d)",
               restart_count, BEACON_GOVERNANCE_MAX_RESTARTS);

    pid_t new_pid = beacon_governance_spawn();
    if (new_pid < 0) {
        /* spawn 失败——不重置计数器，让重试上限生效 */
        return -1;
    }

    /* spawn 成功——验证存活（给子进程一点启动时间） */
    usleep(100000);  /* 100ms */

    if (beacon_governance_is_alive()) {
        /* 成功重启——重置计数器 */
        restart_count = 0;
        beacon_log(BEACON_LOG_INFO, "governance worker restarted successfully (pid=%d)",
                   (int)new_pid);
        return new_pid;
    }

    /* 子进程启动后立即死亡——继续重试 */
    beacon_log(BEACON_LOG_WARN, "governance worker died immediately after spawn");
    return -1;
}

/* ---- 关闭治理 worker ----
 *
 * 1. 发送 SIGTERM
 * 2. waitpid 超时 2 秒
 * 3. 超时则 SIGKILL + waitpid
 *
 * 对标 systemd KillMode=mixed、nginx master kill workers。
 * 设计依据：docs/design/governance-worker.md §5.3 优雅退出
 */
void beacon_governance_shutdown(void)
{
    pid_t pid = BEACON_G(governance_pid);

    if (pid <= 0) {
        /* 从未 spawn 或已清理 */
        return;
    }

    /* 发送 SIGTERM */
    if (kill(pid, SIGTERM) < 0) {
        if (errno == ESRCH) {
            /* 进程已不存在 */
            beacon_log(BEACON_LOG_INFO, "governance worker already exited (pid=%d)", (int)pid);
            BEACON_G(governance_pid) = 0;
            return;
        }
        beacon_log(BEACON_LOG_WARN, "governance shutdown: kill(SIGTERM) failed: %s",
                   strerror(errno));
    }

    /* waitpid 超时等待（轮询方式，避免阻塞） */
    int status;
    pid_t waited = 0;
    int elapsed_usec = 0;
    int poll_interval_usec = 100000;  /* 100ms */

    while (elapsed_usec < BEACON_WAITPID_TIMEOUT_USEC) {
        waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid || waited == -1) {
            break;  /* 子进程已退出或出错 */
        }
        usleep(poll_interval_usec);
        elapsed_usec += poll_interval_usec;
    }

    if (waited == pid) {
        /* SIGTERM 成功退出 */
        beacon_log(BEACON_LOG_INFO, "governance worker exited gracefully (pid=%d, status=%d)",
                   (int)pid, WEXITSTATUS(status));
    } else {
        /* 超时——SIGKILL 强制终止 */
        beacon_log(BEACON_LOG_WARN, "governance worker did not exit in %dms, sending SIGKILL",
                   BEACON_WAITPID_TIMEOUT_USEC / 1000);

        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);  /* 阻塞等待回收 */

        beacon_log(BEACON_LOG_WARN, "governance worker killed (pid=%d)", (int)pid);
    }

    /* 清理 shm header 中的 pid */
    beacon_shm_t *shm = BEACON_G(shm);
    if (shm) {
        shm->header.governance_pid = 0;
    }

    BEACON_G(governance_pid) = 0;
    restart_count = 0;
}
