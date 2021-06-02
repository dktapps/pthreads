--TEST--
Tests basic functionality of Thread::setAutoloadFile()
--FILE--
<?php

Thread::setAutoloadFile(__DIR__ . '/assets/TestAutoloadFile.php');

$t = new class extends \Thread{
	public function run() : void{
		(new TestAutoloadClass())->hi();
	}
};
$t->start(PTHREADS_INHERIT_NONE);
$t->join();
?>
--EXPECT--
string(20) "everything is great!"
