--TEST--
Governance rejects malformed nodes without field contamination
--SKIPIF--
<?php if (!extension_loaded("beacon")) die("skip beacon not loaded"); ?>
--INI--
beacon.log_level=error
beacon.enabled=1
beacon.shm_key=1896415401
beacon.log_level=error
--ENV--
BEACON_GOVERNANCE_WORKER=1
--FILE--
<?php

use Beacon\Governance as G;
$warnings = 0;
set_error_handler(function() use (&$warnings) { ++$warnings; return true; });
$valid = ['id'=>'valid', 'host'=>'127.0.0.1', 'port'=>65535];
$bad = $valid; $bad['id']='bad'; $bad['port']=0; $bad['methods']='leaked';
$nodes=[$bad, $valid, ['id'=>'missing-port','host'=>'localhost'], 42];
foreach (['id', 'host'] as $field) {
    foreach (['', str_repeat('x', 64), "x\0y", 12] as $value) {
        $n=$valid; $n[$field]=$value; $nodes[]=$n;
    }
}
foreach ([-1, 65536, '1'] as $weight) { $n=$valid; $n['weight']=$weight; $nodes[]=$n; }
foreach (['unknown', 256, -1, true, "ok\0bad"] as $status) { $n=$valid; $n['status']=$status; $nodes[]=$n; }
G::storeNodes('validation', $nodes); G::commit();
restore_error_handler();
echo 'warnings=', $warnings, "\n";
var_export(Beacon::getInstances('validation')); echo "\n";

?>
--EXPECT--
warnings=19
array (
  0 => 
  array (
    'id' => 'valid',
    'host' => '127.0.0.1',
    'port' => 65535,
    'status' => 'ok',
    'weight' => 1,
  ),
)
