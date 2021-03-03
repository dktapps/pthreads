--TEST--
New class after thread creation
--DESCRIPTION--
Fixes bug #711. This requires a class to be declared after a new thread has been
created, where the new class implements at least one interface
--FILE--
<?php

$worker = new \Worker();
$worker->start();

interface A {}
class task extends ThreadedRunnable implements A {
	public function run() {}
}

$worker->stack(new task());
$worker->shutdown();
--EXPECT--
