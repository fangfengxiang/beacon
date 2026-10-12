--TEST--
Service name boundaries do not alias valid service names
--SKIPIF--
<?php if (!extension_loaded("beacon")) die("skip beacon not loaded"); ?>
--INI--
beacon.log_level=error
beacon.enabled=1
beacon.shm_key=1896415402
beacon.log_level=error
--ENV--
BEACON_GOVERNANCE_WORKER=1
--FILE--
<?php

use Beacon\Governance as G;
$n=['id'=>'n', 'host'=>'localhost', 'port'=>1];
$svc=str_repeat('s',63);
var_dump(G::storeNodes($svc,[$n]), G::commit());
$warnings=0;
set_error_handler(function() use (&$warnings) { ++$warnings; return true; });
foreach (['', $svc.'x', "{$svc}\0suffix"] as $bad) {
    var_dump(G::storeNodes($bad,[$n]), Beacon::pick($bad), Beacon::getInstances($bad));
}
restore_error_handler();
echo 'warnings=', $warnings, "\n";
echo 'valid=',count(Beacon::getInstances($svc)),"\n";

?>
--EXPECT--
bool(true)
bool(true)
bool(false)
NULL
array(0) {
}
bool(false)
NULL
array(0) {
}
bool(false)
NULL
array(0) {
}
warnings=9
valid=1
