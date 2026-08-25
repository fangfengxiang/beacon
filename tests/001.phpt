--TEST--
Check for beacon extension presence
--SKIPIF--
<?php
if (!extension_loaded("beacon")) {
    print "skip";
}
?>
--FILE--
<?php
echo "beacon extension is available\n";
echo "version: " . phpversion("beacon") . "\n";
?>
--EXPECT--
beacon extension is available
version: 0.1.0-dev
