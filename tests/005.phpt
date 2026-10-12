--TEST--
Health calculation calibrates synthetic busy counter drift
--SKIPIF--
<?php
if (!extension_loaded("beacon")) {
    print "skip";
}
if (!function_exists("beacon_test_set_pool_counters")) {
    print "skip";
}
?>
--INI--
beacon.enabled=1
beacon.log_level=error
beacon.shm_key=0x71080005
--ENV--
BEACON_GOVERNANCE_WORKER=1
--FILE--
<?php
require __DIR__ . "/beacon.inc";

beacon_test_set_pool_counters(8, 2, 1000);

$h = beacon_test_health();
echo "pool_busy=" . $h["pool_busy"] . "\n";
echo "pool_idle=" . $h["pool_idle"] . "\n";
echo "pool_total=" . $h["pool_total"] . "\n";
echo "saturation=" . round($h["saturation"], 1) . "\n";
?>
--EXPECT--
pool_busy=1
pool_idle=2
pool_total=1000
saturation=0.3
