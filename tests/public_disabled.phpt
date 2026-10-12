--TEST--
Disabled public API returns documented degraded values and warnings
--SKIPIF--
<?php if (!extension_loaded("beacon")) die("skip beacon not loaded"); ?>
--INI--
beacon.log_level=error
beacon.enabled=0
--FILE--
<?php

$warnings=0;
set_error_handler(function() use (&$warnings) { ++$warnings; return true; });
var_export(Beacon::status()); echo "\n";
var_dump(Beacon::pick('x'),Beacon::getInstances('x'),Beacon::ready(),Beacon::deregister(),Beacon::reportHealth(['status'=>'ok']),Beacon\Governance::storeNodes('x',[]),Beacon\Governance::commit());
echo 'warnings=',$warnings,"\n";
restore_error_handler();

?>
--EXPECT--
array (
  'enabled' => false,
  'mode' => 'degraded',
  'health' => 'not_ready',
  'governance_alive' => false,
  'governance_pid' => 0,
  'cache_age_seconds' => -1,
)
NULL
array(0) {
}
bool(false)
bool(false)
bool(false)
bool(false)
bool(false)
warnings=7
