--TEST--
Public health reports readiness, metrics and saturation transitions
--SKIPIF--
<?php if (!extension_loaded("beacon")) die("skip beacon not loaded"); ?>
--INI--
beacon.enabled=1
beacon.shm_key=1896415309
beacon.log_level=error
--ENV--
BEACON_GOVERNANCE_WORKER=1
--FILE--
<?php

use Beacon\Governance as G;
echo G::calcHealth()['status'],"\n";
var_dump(Beacon::ready());
beacon_test_set_pool_counters(1,9,100);
$h=G::calcHealth();
echo $h['status'],':',$h['pool_busy'],':',$h['pool_idle'],':',$h['pool_total'],':',$h['saturation'],"\n";
var_dump($h['governance_alive']);
beacon_test_set_pool_counters(2,0,101);
echo G::calcHealth()['status'],"\n";
var_dump(Beacon::status()['pool_ready']);

?>
--EXPECT--
not_ready
bool(true)
ok:1:9:100:0.1
bool(true)
degraded
bool(true)
