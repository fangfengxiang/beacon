--TEST--
beacon_test_pick() exclude list and failover
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
beacon.shm_key=0x71080008
--FILE--
<?php
require __DIR__ . "/beacon.inc";

$nodes = [
    beacon_test_node("ex-1", "10.0.0.1", 8888, BEACON_NODE_OK, 1),
    beacon_test_node("ex-2", "10.0.0.2", 8888, BEACON_NODE_OK, 1),
    beacon_test_node("ex-3", "10.0.0.3", 8888, BEACON_NODE_OK, 1),
];
beacon_test_store("ex-svc", $nodes);

/* Pick without exclude — should return a valid node */
$node = beacon_test_pick("ex-svc", BEACON_LB_ROUND_ROBIN, [], false);
echo "no_exclude_is_null=" . ($node === null ? "yes" : "no") . "\n";
echo "no_exclude_id=" . ($node ? $node["id"] : "none") . "\n";

/* Pick with one excluded — should return a different node */
$first_id = $node ? $node["id"] : "ex-1";
$node2 = beacon_test_pick("ex-svc", BEACON_LB_ROUND_ROBIN, [$first_id], false);
echo "one_exclude_is_null=" . ($node2 === null ? "yes" : "no") . "\n";
echo "one_exclude_different=" . ($node2 && $node2["id"] !== $first_id ? "yes" : "no") . "\n";

/* Pick with all nodes excluded — should return null */
$node3 = beacon_test_pick("ex-svc", BEACON_LB_ROUND_ROBIN,
    ["ex-1", "ex-2", "ex-3"], false);
echo "all_excluded_is_null=" . ($node3 === null ? "yes" : "no") . "\n";

/* Pick from non-existent service — should return null */
$node4 = beacon_test_pick("no-such-svc", BEACON_LB_ROUND_ROBIN, [], false);
echo "missing_service_is_null=" . ($node4 === null ? "yes" : "no") . "\n";
?>
--EXPECTF--
no_exclude_is_null=no
no_exclude_id=%s
one_exclude_is_null=no
one_exclude_different=yes
all_excluded_is_null=yes
missing_service_is_null=yes
