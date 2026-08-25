--TEST--
beacon_test_pick() round-robin strategy
--SKIPIF--
<?php
if (!extension_loaded("beacon")) {
    print "skip";
}
if (!function_exists("beacon_test_pick")) {
    print "skip";
}
?>
--INI--
beacon.enabled=1
beacon.shm_key=0x71080007
--FILE--
<?php
require __DIR__ . "/beacon.inc";

$nodes = [
    beacon_test_node("rr-1", "10.0.0.1", 8888, BEACON_NODE_OK, 1),
    beacon_test_node("rr-2", "10.0.0.2", 8888, BEACON_NODE_OK, 1),
    beacon_test_node("rr-3", "10.0.0.3", 8888, BEACON_NODE_OK, 1),
];
beacon_test_store("rr-svc", $nodes);

$picked = [];
for ($i = 0; $i < 3; $i++) {
    $node = beacon_test_pick("rr-svc", BEACON_LB_ROUND_ROBIN, [], false);
    if ($node !== null) {
        $picked[] = $node["id"];
    }
}
echo "picked=" . implode(",", $picked) . "\n";
echo "unique=" . count(array_unique($picked)) . "\n";
?>
--EXPECTF--
picked=%s
unique=3
