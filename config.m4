dnl config.m4 for php-beacon-extension
dnl
dnl PHP-FPM 状态信标与治理调度基底扩展
dnl 构建：phpize && ./configure [--enable-beacon] && make

PHP_ARG_ENABLE([beacon],
  [whether to enable beacon support],
  [AS_HELP_STRING([--enable-beacon],
    [Enable php-beacon-extension: FPM state beacon and governance scheduling substrate])],
  [no])

if test "$PHP_BEACON" != "no"; then

  dnl ---- 探测 PHP 版本 >= 8.0（C11 atomics 需要现代编译器 + PHP 8 Zend API）----
  AC_MSG_CHECKING([PHP version >= 8.0])
  php_version=`$PHP_CONFIG --version 2>/dev/null`
  if test -z "$php_version"; then
    AC_MSG_ERROR([cannot determine PHP version, is php-config in PATH?])
  fi
  php_major=`echo "$php_version" | $SED 's/\..*//'`
  if test "$php_major" -lt 8; then
    AC_MSG_ERROR([beacon requires PHP >= 8.0, found $php_version])
  fi
  AC_MSG_RESULT([yes ($php_version)])

  dnl ---- 探测 C11 <stdatomic.h>（原子自计数依赖）----
  AC_CHECK_HEADER([stdatomic.h], [], [
    AC_MSG_ERROR([<stdatomic.h> not found, beacon requires a C11-capable compiler (gcc >= 4.9 / clang >= 3.6)])
  ])

  dnl ---- 探测 sysv shm（跨进程共享内存）----
  AC_CHECK_HEADER([sys/shm.h],
    [AC_DEFINE([HAVE_SYSV_SHM], [1], [whether sysv shm is available])],
    [AC_MSG_ERROR([<sys/shm.h> not found, beacon requires sysv shared memory])])

  dnl ---- 探测 sys/ipc.h（shmget IPC key）----
  AC_CHECK_HEADER([sys/ipc.h], [],
    [AC_MSG_ERROR([<sys/ipc.h> not found])])

  dnl ---- 探测 prctl（Linux 父进程死亡信号，治理 worker 用）----
  AC_CHECK_HEADER([sys/prctl.h],
    [AC_DEFINE([HAVE_PRCTL], [1], [whether prctl is available (Linux)])],
    [AC_MSG_WARN([<sys/prctl.h> not found, governance worker prctl fallback to getppid polling])])

  dnl ---- 探测 dirent.h（fd 目录遍历，治理 worker exec 前清理用）----
  dnl Linux 遍历 /proc/self/fd，macOS/BSD 遍历 /dev/fd（fdesc 默认挂载）
  AC_CHECK_HEADERS([dirent.h], [], [AC_MSG_ERROR([<dirent.h> not found])])

  dnl ---- 探测 CRC32（用 zlib 或内置实现）----
  AC_CHECK_HEADER([zlib.h],
    [AC_DEFINE([HAVE_ZLIB_CRC32], [1], [whether zlib crc32 is available])
     PHP_ADD_LIBRARY([z], [1], [BEACON_SHARED_LIBADD])],
    [AC_MSG_WARN([<zlib.h> not found, will use built-in CRC32 implementation])])

  dnl ---- 编译宏：PHP CLI 路径（治理 worker exec 用）----
  dnl 推导链：环境变量 PHP_BEACON_PHP_BIN > php-config --php-binary > which php > /usr/bin/php
  dnl 优先 php-config --php-binary：保证治理 worker 的 PHP 版本与扩展编译版本一致
  dnl （/usr/bin/php 在 Homebrew/源码编译/多版本共存场景下可能不存在或版本不符）
  PHP_BEACON_PHP_BIN=${PHP_BEACON_PHP_BIN:-$($PHP_CONFIG --php-binary 2>/dev/null)}
  if test ! -x "$PHP_BEACON_PHP_BIN"; then
    dnl 老 php-config 不支持 --php-binary 时 usage 输出到 stdout，-x 检查拦截该污染
    PHP_BEACON_PHP_BIN=$(which php 2>/dev/null)
  fi
  if test -z "$PHP_BEACON_PHP_BIN"; then
    AC_MSG_WARN([php binary not found, set PHP_BEACON_PHP_BIN at compile time or beacon.governance_bin at runtime])
    PHP_BEACON_PHP_BIN="/usr/bin/php"
  fi
  AC_MSG_NOTICE([beacon: php binary = $PHP_BEACON_PHP_BIN])
  AC_DEFINE_UNQUOTED([BEACON_PHP_BIN], ["$PHP_BEACON_PHP_BIN"], [path to php CLI binary for governance worker exec])

  dnl ---- 编译宏：数据目录（内置 governance.php 安装路径）----
  dnl 推导链：环境变量 PHP_BEACON_DATA_DIR > php-config --prefix 推导
  dnl share/php/beacon 对齐 PEAR share/php/pear 惯例
  PHP_BEACON_DATA_DIR=${PHP_BEACON_DATA_DIR:-"$($PHP_CONFIG --prefix)/share/php/beacon"}
  AC_MSG_NOTICE([beacon: data dir = $PHP_BEACON_DATA_DIR])
  AC_DEFINE_UNQUOTED([BEACON_DATA_DIR], ["$PHP_BEACON_DATA_DIR"], [data directory for built-in governance script])

  dnl ---- 注册扩展源文件（按分层顺序，依赖方向单向）----
  dnl Infrastructure: config / log / shm
  dnl Domain Service: health / select (Phase 2)
  dnl Lifecycle: governance_worker / callback (Phase 3)
  dnl API: api / api_governance (Phase 4)
  dnl Entry: beacon
  dnl extra-cflags 直接写字面量：STANDARD_CFLAGS 由 PHP 构建系统自带，
  dnl $(STANDARD_CFLAGS) 写法会在 configure 阶段被当命令执行（command not found）
  PHP_NEW_EXTENSION([beacon],
    [beacon_config.c beacon_log.c beacon_shm.c beacon_service_health.c beacon_service_select.c beacon_governance_worker.c beacon_callback.c beacon_api.c beacon_api_governance.c beacon.c],
    [$ext_shared],,
    [-D_GNU_SOURCE -std=c11 -Wall -Wextra])

  PHP_SUBST([BEACON_SHARED_LIBADD])
  PHP_INSTALL_HEADERS([ext/beacon], [php_beacon.h beacon_shm.h beacon_log.h])

  dnl ---- 安装内置 governance.php 脚本（make install 钩子，规则在 Makefile.frag）----
  BEACON_DATA_DIR=$PHP_BEACON_DATA_DIR
  PHP_SUBST([BEACON_DATA_DIR])
  PHP_ADD_MAKEFILE_FRAGMENT()
fi
