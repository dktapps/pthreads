--TEST--
Test that strings fetched from Threaded properties don't leak
--FILE--
<?php

$threaded = new \Threaded();
$threaded->test = str_repeat("a", 10);
var_dump($threaded->test);

?>
--EXPECT--
string(10) "aaaaaaaaaa"
