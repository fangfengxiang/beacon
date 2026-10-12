--TEST--
Public classes expose static signatures and Zend argument errors
--SKIPIF--
<?php if (!extension_loaded("beacon")) die("skip beacon not loaded"); ?>
--INI--
beacon.enabled=0
--FILE--
<?php

foreach (['Beacon'=>['pick'=>1,'getInstances'=>1,'ready'=>0,'status'=>0,'setOpt'=>2,'deregister'=>0,'reportHealth'=>1], 'Beacon\\Governance'=>['storeNodes'=>2,'commit'=>0,'calcHealth'=>0]] as $class=>$methods) {
    foreach ($methods as $name=>$required) {
        $m=new ReflectionMethod($class,$name);
        echo "$class::$name ", ($m->isStatic() && $m->isPublic() && $m->getNumberOfRequiredParameters()===$required ? 'ok':'FAIL'),"\n";
    }
}
foreach ([fn()=>Beacon::pick(), fn()=>Beacon::getInstances([]), fn()=>Beacon::setOpt(1), fn()=>Beacon::ready(1), fn()=>Beacon::reportHealth('bad'), fn()=>Beacon\Governance::storeNodes('x','bad'), fn()=>Beacon\Governance::commit(1)] as $call) {
    try { $call(); echo "FAIL\n"; } catch (TypeError $e) { echo get_class($e),"\n"; }
}

?>
--EXPECT--
Beacon::pick ok
Beacon::getInstances ok
Beacon::ready ok
Beacon::status ok
Beacon::setOpt ok
Beacon::deregister ok
Beacon::reportHealth ok
Beacon\Governance::storeNodes ok
Beacon\Governance::commit ok
Beacon\Governance::calcHealth ok
ArgumentCountError
TypeError
ArgumentCountError
ArgumentCountError
TypeError
TypeError
ArgumentCountError
