--TEST--
Callback replacement, clear, exceptions and self removal release references
--SKIPIF--
<?php if (!extension_loaded("beacon")) die("skip beacon not loaded"); ?>
--INI--
beacon.log_level=error
beacon.enabled=0
--FILE--
<?php

$object=new stdClass;
$weak=WeakReference::create($object);
Beacon::setOpt(Beacon::OPT_ON_REGISTER,function($ctx) use ($object) { echo "first=$ctx\n"; });
unset($object);
var_dump($weak->get() !== null);
beacon_test_callback_invoke(1,42);
Beacon::setOpt(Beacon::OPT_ON_REGISTER,function($ctx) { echo "replacement=$ctx\n"; });
var_dump($weak->get());
beacon_test_callback_invoke(1,7);
Beacon::setOpt(Beacon::OPT_ON_REGISTER,function($ctx) { throw new RuntimeException('isolated'); });
var_dump(beacon_test_callback_invoke(1,0));
Beacon::setOpt(Beacon::OPT_ON_REGISTER,function($ctx) {
    Beacon::setOpt(Beacon::OPT_ON_REGISTER,null);
    echo "self-cleared=$ctx\n";
});
var_dump(beacon_test_callback_invoke(1,9),beacon_test_callback_invoke(1,9));

?>
--EXPECT--
bool(true)
first=42
NULL
replacement=7
int(-1)
self-cleared=9
int(0)
int(-1)
