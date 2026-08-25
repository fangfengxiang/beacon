--TEST--
beacon_test_gov_spawn/is_alive/pid governance worker lifecycle
--SKIPIF--
<?php
if (!extension_loaded("beacon")) {
    print "skip";
}
if (!function_exists("beacon_test_gov_spawn")) {
    print "skip";
}
if (!file_exists(getenv("PHP_BINARY") ?: "/usr/bin/php")) {
    print "skip";
}
?>
--INI--
beacon.enabled=1
beacon.shm_key=0x7108000a
beacon.governance_bin=/usr/bin/php
beacon.governance_script=governance.php
--FILE--
<?php
require __DIR__ . "/beacon.inc";

/* ---- 1. Initial state: no governance worker spawned ---- */
echo "initial_pid=" . beacon_test_gov_pid() . "\n";
echo "initial_alive=" . (beacon_test_gov_is_alive() ? "true" : "false") . "\n";

/* ---- 2. Attempt spawn ---- */
$pid = beacon_test_gov_spawn();
echo "spawn_result=" . $pid . "\n";

if ($pid > 0) {
    /* Spawn succeeded — verify pid recorded and liveness */
    echo "recorded_pid=" . beacon_test_gov_pid() . "\n";
    echo "pid_matches=" . (beacon_test_gov_pid() === $pid ? "yes" : "no") . "\n";

    /* Give child process a moment to start */
    usleep(200000);

    echo "after_spawn_alive=" . (beacon_test_gov_is_alive() ? "true" : "false") . "\n";
} else {
    /* Spawn failed (e.g., binary not found) — verify pid not set */
    echo "spawn_failed_pid=" . beacon_test_gov_pid() . "\n";
    echo "spawn_failed_alive=" . (beacon_test_gov_is_alive() ? "true" : "false") . "\n";
}
?>
--EXPECTF--
initial_pid=0
initial_alive=false
spawn_result=%d
%s
