--TEST--
Governance node capacity and optional field lengths stay bounded
--SKIPIF--
<?php if (!extension_loaded("beacon")) die("skip beacon not loaded"); ?>
--INI--
beacon.enabled=1
beacon.shm_key=1896415310
beacon.log_level=error
--ENV--
BEACON_GOVERNANCE_WORKER=1
--FILE--
<?php

use Beacon\Governance as G;
$nodes=[];
for($i=0;$i<65;++$i) $nodes[]=['id'=>'n'.$i,'host'=>str_repeat('h',63),'port'=>1,'methods'=>str_repeat('m',300)];
var_dump(G::storeNodes('capacity',$nodes),G::commit());
$result=Beacon::getInstances('capacity');
echo count($result),':',strlen($result[0]['host']),':',strlen($result[0]['methods']),"\n";
echo $result[63]['id'],"\n";
G::storeNodes('capacity',[$nodes[0]]);G::commit();echo count(Beacon::getInstances('capacity')),"\n";

?>
--EXPECT--
bool(true)
bool(true)
64:63:255
n63
1
