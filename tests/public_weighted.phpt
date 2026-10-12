--TEST--
Weighted routing excludes zero weights without probabilistic expectations
--SKIPIF--
<?php if (!extension_loaded("beacon")) die("skip beacon not loaded"); ?>
--INI--
beacon.log_level=error
beacon.enabled=1
beacon.shm_key=1896415403
beacon.log_level=error
--ENV--
BEACON_GOVERNANCE_WORKER=1
--FILE--
<?php

use Beacon\Governance as G;
$nodes=[['id'=>'zero','host'=>'localhost','port'=>1,'weight'=>0], ['id'=>'positive','host'=>'localhost','port'=>2,'weight'=>65535]];
G::storeNodes('weighted',$nodes); G::commit();
$ids=[];
for ($i=0; $i<128; ++$i) $ids[Beacon::pick('weighted',[Beacon::OPT_LB_STRATEGY=>Beacon::LB_WEIGHTED])['id']]=true;
echo implode(',',array_keys($ids)),"\n";
$nodes[1]['weight']=0; G::storeNodes('weighted',$nodes); G::commit();
var_dump(Beacon::pick('weighted',[Beacon::OPT_LB_STRATEGY=>Beacon::LB_WEIGHTED]));

?>
--EXPECT--
positive
NULL
