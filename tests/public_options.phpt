--TEST--
Public setOpt validates strategy and positive numeric options
--SKIPIF--
<?php if (!extension_loaded("beacon")) die("skip beacon not loaded"); ?>
--INI--
beacon.log_level=error
beacon.enabled=0
--FILE--
<?php

set_error_handler(function() { return true; });
foreach ([0,4,-1,PHP_INT_MAX,'1',null] as $v) var_dump(Beacon::setOpt(Beacon::OPT_LB_STRATEGY,$v));
foreach ([1,2,3] as $v) var_dump(Beacon::setOpt(Beacon::OPT_LB_STRATEGY,$v));
foreach ([7,8,9,10] as $key) {
    var_dump(Beacon::setOpt($key,1));
    foreach ([0,-1,'1',null] as $v) var_dump(Beacon::setOpt($key,$v));
}
foreach ([11,12,999] as $key) var_dump(Beacon::setOpt($key,[]));
var_dump(Beacon::setOpt(Beacon::OPT_ON_REGISTER,'missing_callable'),Beacon::setOpt(Beacon::OPT_ON_REGISTER,null));
restore_error_handler();

?>
--EXPECT--
bool(false)
bool(false)
bool(false)
bool(false)
bool(false)
bool(false)
bool(true)
bool(true)
bool(true)
bool(true)
bool(false)
bool(false)
bool(false)
bool(false)
bool(true)
bool(false)
bool(false)
bool(false)
bool(false)
bool(true)
bool(false)
bool(false)
bool(false)
bool(false)
bool(true)
bool(false)
bool(false)
bool(false)
bool(false)
bool(false)
bool(false)
bool(false)
bool(false)
bool(true)
