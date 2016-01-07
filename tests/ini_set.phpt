--TEST--
Check for ini_set("pinba.server", ...)
--SKIPIF--
<?php if (!extension_loaded("pinba")) print "skip"; ?>
--FILE--
<?php
echo "pinba extension is available\n";
var_export(ini_get("pinba.server"));
echo "\n\n";

var_export(ini_set("pinba.server", "one.server"));
echo "\n";
var_export(ini_get("pinba.server"));
echo "\n\n";

var_export(ini_set("pinba.server", "one.server,two.server"));
echo "\n";
var_export(ini_get("pinba.server"));
echo "\n\n";

var_export(ini_set("pinba.server", ""));
echo "\n";
var_export(ini_get("pinba.server"));
echo "\n";
?>
--EXPECT--
pinba extension is available
''

''
'one.server'

'one.server'
'one.server,two.server'

'one.server,two.server'
''