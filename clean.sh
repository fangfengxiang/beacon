#!/bin/sh
# clean.sh — 清理 PHP 扩展编译产物
#
# 用法: ./clean.sh
#
# 清理内容（与 .gitignore 对齐）：
#   - make distclean（若 Makefile 存在）
#   - phpize --clean（若 configure 存在）
#   - 残留编译产物（.dep/.o/.lo/.so/.la 等）

set -e

cd "$(dirname "$0")"

# 1. make distclean（清理编译产物 + Makefile）
if [ -f Makefile ]; then
    echo ">> make distclean"
    make distclean 2>/dev/null || true
fi

# 2. phpize --clean（清理 configure/config.h 等 phpize 生成物）
if [ -f configure ]; then
    echo ">> phpize --clean"
    phpize --clean 2>/dev/null || true
fi

# 3. 清理残留（phpize --clean 可能漏掉的）
echo ">> removing residual build artifacts"
rm -rf autom4te.cache build modules .deps
rm -f  config.h config.h.in config.log config.status
rm -f  configure configure.ac
rm -f  libtool mkinstalldirs run-tests.php tmp-php.ini
rm -f  Makefile Makefile.fragments Makefile.global Makefile.objects
rm -f  *.dep *.o *.lo *.slo *.la *.lai *.a *.so

# 4. 清理 phpt 测试运行产物（run-tests.php 生成的 .out/.exp/.diff/.sh/.php/.log）
echo ">> removing phpt test artifacts"
rm -f  tests/*.out tests/*.exp tests/*.diff tests/*.sh tests/*.php tests/*.log

echo ">> done"
