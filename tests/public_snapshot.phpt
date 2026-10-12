--TEST--
Public governance commits isolate staged data and preserve other services
--SKIPIF--
<?php if (!extension_loaded("beacon")) die("skip beacon not loaded"); ?>
--INI--
beacon.log_level=error
beacon.enabled=1
beacon.shm_key=1896415400
beacon.log_level=error
--ENV--
BEACON_GOVERNANCE_WORKER=1
--FILE--
<?php

use Beacon\Governance as G;
function node($id) { return ['id'=>$id, 'host'=>'127.0.0.1', 'port'=>9000]; }
function ids($svc) { echo $svc, '=', implode(',', array_column(Beacon::getInstances($svc), 'id')), "\n"; }
G::storeNodes('a', [node('a1')]); ids('a');
G::storeNodes('b', [node('b1')]); G::commit(); ids('a'); ids('b');
G::storeNodes('a', [node('a2')]); ids('a'); G::commit(); ids('a'); ids('b');
G::storeNodes('a', []); G::commit(); ids('a'); ids('b');
G::commit(); ids('a'); ids('b');
var_dump(Beacon::pick('a'));

?>
--EXPECT--
a=
a=a1
b=b1
a=a1
a=a2
b=b1
a=
b=b1
a=
b=b1
NULL
