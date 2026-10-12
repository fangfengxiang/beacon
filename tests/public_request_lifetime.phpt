--TEST--
Callbacks do not survive consecutive requests in one PHP server process
--SKIPIF--
<?php
if (!extension_loaded('beacon')) die('skip beacon not loaded');
if (!function_exists('proc_open')) die('skip proc_open required');
?>
--INI--
beacon.enabled=0
--FILE--
<?php
$socket = stream_socket_server('tcp://127.0.0.1:0', $errno, $error);
$address = stream_socket_get_name($socket, false);
fclose($socket);
$cmd = [PHP_BINARY, '-n', '-d', 'extension=' . dirname(__DIR__) . '/modules/beacon.so', '-d', 'beacon.enabled=0', '-S', $address, __DIR__ . '/fixtures/request_router.inc'];
$process = proc_open($cmd, [['pipe','r'],['pipe','w'],['pipe','w']], $pipes);
try {
    for ($i = 0; $i < 100; ++$i) {
        $probe = @stream_socket_client('tcp://' . $address, $errno, $error, 0.05);
        if ($probe) { fclose($probe); break; }
        usleep(20000);
    }
    foreach (['first', 'second', 'third'] as $path) {
        $connection = stream_socket_client('tcp://' . $address, $errno, $error, 2);
        stream_set_timeout($connection, 2);
        fwrite($connection, "GET /$path HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        $response = stream_get_contents($connection);
        fclose($connection);
        echo explode("\r\n\r\n", $response, 2)[1], "\n";
    }
} finally {
    proc_terminate($process);
    foreach ($pipes as $pipe) fclose($pipe);
    proc_close($process);
}
?>
--EXPECT--
registered
-1
-1
