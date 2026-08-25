--TEST--
beacon_test_health() returns pool health structure
--SKIPIF--
<?php
if (!extension_loaded("beacon")) {
    print "skip";
}
if (!function_exists("beacon_test_health")) {
    print "skip";
}
?>
--FILE--
<?php
$h = beacon_test_health();

echo "is_array: " . (is_array($h) ? "yes" : "no") . "\n";

if (is_array($h)) {
    echo "has_status: " . (array_key_exists("status", $h) ? "yes" : "no") . "\n";
    echo "has_pool_busy: " . (array_key_exists("pool_busy", $h) ? "yes" : "no") . "\n";
    echo "has_pool_idle: " . (array_key_exists("pool_idle", $h) ? "yes" : "no") . "\n";
    echo "has_pool_total: " . (array_key_exists("pool_total", $h) ? "yes" : "no") . "\n";
    echo "has_saturation: " . (array_key_exists("saturation", $h) ? "yes" : "no") . "\n";
    echo "has_governance_alive: " . (array_key_exists("governance_alive", $h) ? "yes" : "no") . "\n";
}
?>
--EXPECTF--
is_array: %s
has_status: %s
has_pool_busy: %s
has_pool_idle: %s
has_pool_total: %s
has_saturation: %s
has_governance_alive: %s
