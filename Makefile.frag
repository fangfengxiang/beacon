# Makefile.frag for php-beacon-extension
#
# 安装内置治理脚本 governance.php 到 BEACON_DATA_DIR（make install 时执行）。
# 机制：无 recipe 的 `install: install-beacon-data` 将依赖合并进
# PHP 构建系统（Makefile.global）的 install 目标，不覆盖其 recipe。
# 对标 ext/phar Makefile.frag 安装 phar.phar 的做法。

install-beacon-data:
	@$(mkinstalldirs) $(INSTALL_ROOT)$(BEACON_DATA_DIR)
	$(INSTALL) -m 0644 $(top_srcdir)/governance.php $(INSTALL_ROOT)$(BEACON_DATA_DIR)/governance.php
	@echo "beacon: installed governance.php to $(INSTALL_ROOT)$(BEACON_DATA_DIR)"

install: install-beacon-data
