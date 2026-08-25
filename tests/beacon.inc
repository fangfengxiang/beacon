<?php
/**
 * beacon.inc — test helper for beacon extension
 *
 * Provides shared constants and helpers for .phpt test files.
 * Modeled after yar.inc in the Yar extension test suite.
 */

/* LB strategy constants (mirror of Beacon::LB_* in C) */
define("BEACON_LB_ROUND_ROBIN", 1);
define("BEACON_LB_RANDOM",      2);
define("BEACON_LB_WEIGHTED",    3);

/* Health status constants (mirror of BEACON_HEALTH_STATUS_* in C) */
define("BEACON_HEALTH_NOT_READY", 0);
define("BEACON_HEALTH_OK",       1);
define("BEACON_HEALTH_DEGRADED", 2);
define("BEACON_HEALTH_DEAD",     3);

/* Node status constants (mirror of BEACON_NODE_STATUS_* in C) */
define("BEACON_NODE_OK",        0);
define("BEACON_NODE_DEGRADED",  1);
define("BEACON_NODE_DEAD",      2);
define("BEACON_NODE_NOT_READY", 3);

/**
 * Build a test node array (for beacon_test_store).
 *
 * @param string $id     Instance ID
 * @param string $host   Host address
 * @param int    $port   Port number
 * @param int    $status Node status (BEACON_NODE_*)
 * @param int    $weight Weight for LB
 * @return array
 */
function beacon_test_node($id, $host, $port, $status = 0, $weight = 1) {
    return [
        "id"     => $id,
        "host"   => $host,
        "port"   => $port,
        "status" => $status,
        "weight" => $weight,
    ];
}

/**
 * Assert helper: check that a value equals expected, print pass/fail.
 *
 * @param mixed  $expected
 * @param mixed  $actual
 * @param string $label
 * @return bool
 */
function beacon_assert_eq($expected, $actual, $label = "") {
    if ($expected === $actual) {
        return true;
    }
    echo "FAIL: $label - expected " . var_export($expected, true) .
         ", got " . var_export($actual, true) . "\n";
    return false;
}
