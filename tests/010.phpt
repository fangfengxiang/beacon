--TEST--
Governance worker environment guard prevents recursive MINIT spawning
--SKIPIF--
<?php if (!extension_loaded('beacon')) die('skip beacon not loaded'); ?>
--INI--
beacon.enabled=1
beacon.shm_key=1896415500
beacon.log_level=error
--ENV--
BEACON_GOVERNANCE_WORKER=1
--FILE--
<?php
var_dump(beacon_test_gov_pid(), beacon_test_gov_is_alive());
var_dump(Beacon::status()['governance_pid']);
?>
--EXPECT--
int(0)
bool(false)
int(0)
