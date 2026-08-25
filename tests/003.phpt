--TEST--
Check beacon INI default values
--SKIPIF--
<?php
if (!extension_loaded("beacon")) {
    print "skip";
}
?>
--FILE--
<?php
echo "enabled=" . ini_get("beacon.enabled") . "\n";
echo "keepalive_interval=" . ini_get("beacon.keepalive_interval") . "\n";
echo "pull_interval=" . ini_get("beacon.pull_interval") . "\n";
echo "heartbeat_ttl=" . ini_get("beacon.heartbeat_ttl") . "\n";
echo "health_dead_threshold=" . ini_get("beacon.health_dead_threshold") . "\n";
echo "lb_strategy=" . ini_get("beacon.lb_strategy") . "\n";
echo "log_level=" . ini_get("beacon.log_level") . "\n";
?>
--EXPECT--
enabled=0
keepalive_interval=3
pull_interval=2
heartbeat_ttl=15
health_dead_threshold=3
lb_strategy=round_robin
log_level=warn
