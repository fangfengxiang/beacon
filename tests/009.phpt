--TEST--
beacon_test_callback_set() and beacon_test_callback_invoke()
--SKIPIF--
<?php
if (!extension_loaded("beacon")) {
    print "skip";
}
if (!function_exists("beacon_test_callback_set")) {
    print "skip";
}
?>
--INI--
beacon.enabled=1
beacon.shm_key=0x71080009
--FILE--
<?php
require __DIR__ . "/beacon.inc";

/* Callback type constants (mirror BEACON_CB_ON_* in C) */
$CB_ON_REGISTER   = 1;
$CB_ON_KEEPALIVE  = 2;

/* ---- 1. Set and invoke a valid callback ---- */
$side_effect = 0;
$ok = beacon_test_callback_set($CB_ON_REGISTER, function($ctx) use (&$side_effect) {
    $side_effect = $ctx;
    return true;
});
echo "set_ok=" . ($ok ? "true" : "false") . "\n";

$result = beacon_test_callback_invoke($CB_ON_REGISTER, 42);
echo "invoke_result=" . $result . "\n";
echo "side_effect=" . $side_effect . "\n";

/* ---- 2. Invoke an unset callback type — should return -1 ---- */
$result2 = beacon_test_callback_invoke($CB_ON_KEEPALIVE, null);
echo "unset_invoke=" . $result2 . "\n";

/* ---- 3. Set a non-callable — should return false ---- */
$bad = beacon_test_callback_set($CB_ON_REGISTER, "not_a_function");
echo "set_non_callable=" . ($bad ? "true" : "false") . "\n";

/* ---- 4. Invoke with context array ---- */
beacon_test_callback_set($CB_ON_KEEPALIVE, function($ctx) {
    return $ctx["action"] ?? "none";
});
$result3 = beacon_test_callback_invoke($CB_ON_KEEPALIVE, ["action" => "pull"]);
echo "array_ctx_result=" . $result3 . "\n";
?>
--EXPECT--
set_ok=true
invoke_result=0
side_effect=42
unset_invoke=-1
set_non_callable=false
array_ctx_result=0
