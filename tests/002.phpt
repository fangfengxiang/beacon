--TEST--
Check beacon INI directives
--SKIPIF--
<?php
if (!extension_loaded("beacon")) {
    print "skip";
}
?>
--FILE--
<?php
$inis = [
    "beacon.enabled",
    "beacon.service_name",
    "beacon.advertise_host",
    "beacon.advertise_host_env",
    "beacon.advertise_port",
    "beacon.registry_endpoint",
    "beacon.governance_bin",
    "beacon.governance_script",
    "beacon.keepalive_interval",
    "beacon.pull_interval",
    "beacon.heartbeat_ttl",
    "beacon.health_dead_threshold",
    "beacon.lb_strategy",
    "beacon.shm_key",
    "beacon.log_file",
    "beacon.log_level",
];

foreach ($inis as $ini) {
    $val = ini_get($ini);
    if ($val === false) {
        echo "MISSING: $ini\n";
    } else {
        echo "OK: $ini\n";
    }
}
?>
--EXPECTF--
OK: beacon.enabled
OK: beacon.service_name
OK: beacon.advertise_host
OK: beacon.advertise_host_env
OK: beacon.advertise_port
OK: beacon.registry_endpoint
OK: beacon.governance_bin
OK: beacon.governance_script
OK: beacon.keepalive_interval
OK: beacon.pull_interval
OK: beacon.heartbeat_ttl
OK: beacon.health_dead_threshold
OK: beacon.lb_strategy
OK: beacon.shm_key
OK: beacon.log_file
OK: beacon.log_level
