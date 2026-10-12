--TEST--
Public routing filters node health and applies excludes and strategies
--SKIPIF--
<?php if (!extension_loaded("beacon")) die("skip beacon not loaded"); ?>
--INI--
beacon.log_level=error
beacon.enabled=1
beacon.shm_key=1896415404
beacon.log_level=error
--ENV--
BEACON_GOVERNANCE_WORKER=1
--FILE--
<?php

use Beacon\Governance as G;
$nodes=[];
foreach (['ok','degraded','dead','not_ready'] as $status) $nodes[]=['id'=>$status,'host'=>'localhost','port'=>9000,'status'=>$status,'methods'=>'ping'];
G::storeNodes('routing',$nodes); G::commit();
echo implode(',',array_column(Beacon::getInstances('routing'),'id')),"\n";
echo Beacon::pick('routing')['id'],"\n";
echo Beacon::pick('routing',[Beacon::OPT_EXCLUDE=>['ok']])['id'],"\n";
var_dump(Beacon::pick('routing',[Beacon::OPT_EXCLUDE=>['ok','degraded']]));
$seen=[];
for($i=0;$i<2;++$i) $seen[]=Beacon::pick('routing',[Beacon::OPT_PREFER_HEALTHY=>false])['id'];
sort($seen); echo implode(',',$seen),"\n";
foreach ([Beacon::LB_ROUND_ROBIN,Beacon::LB_RANDOM,Beacon::LB_WEIGHTED] as $strategy) {
    echo Beacon::pick('routing',[Beacon::OPT_LB_STRATEGY=>$strategy])['methods'],"\n";
}
var_dump(Beacon::pick('missing'), Beacon::getInstances('missing'));

?>
--EXPECT--
ok,degraded
ok
degraded
NULL
degraded,ok
ping
ping
ping
NULL
array(0) {
}
