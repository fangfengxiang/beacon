--TEST--
beacon_test_store() and beacon_test_instances() basic
--SKIPIF--
<?php
if (!extension_loaded("beacon")) {
    print "skip";
}
if (!function_exists("beacon_test_store")) {
    print "skip";
}
?>
--INI--
beacon.enabled=1
beacon.shm_key=0x71080006
--FILE--
<?php
require __DIR__ . "/beacon.inc";

$nodes = [
    beacon_test_node("node-1", "10.0.0.1", 8888, BEACON_NODE_OK, 1),
    beacon_test_node("node-2", "10.0.0.2", 8888, BEACON_NODE_OK, 1),
    beacon_test_node("node-3", "10.0.0.3", 8888, BEACON_NODE_OK, 1),
];

$ok = beacon_test_store("user", $nodes);
echo "store=" . ($ok ? "true" : "false") . "\n";

$instances = beacon_test_instances("user");
echo "count=" . count($instances) . "\n";

if (count($instances) > 0) {
    echo "first_id=" . $instances[0]["id"] . "\n";
    echo "first_host=" . $instances[0]["host"] . "\n";
    echo "first_port=" . $instances[0]["port"] . "\n";
}
?>
--EXPECT--
store=true
count=3
first_id=node-1
first_host=10.0.0.1
first_port=8888
