# Linear contraptions (U20): mechanical pistons, rope / elevator pulleys, elevator contacts and
# gantries. Every pistons selftest, then a piston driven from the console through the kinetic
# network (U2's test generator) with its log lines.
$mark = Get-LogMark
$r = Invoke-SelfTest 'pistons.*'
if ($r.Pass -lt 13) { throw "expected at least 13 pistons selftests to pass, got $($r.Pass)" }
Wait-LogLine 'piston at -?\d+ -?\d+ -?\d+ assembled: \d+ blocks' 10 -Since $mark | Out-Null
Wait-LogLine 'pulley at -?\d+ -?\d+ -?\d+ disassembled at offset 6' 10 -Since $mark | Out-Null
Wait-LogLine 'gantry carriage at -?\d+ -?\d+ -?\d+ assembled' 10 -Since $mark | Out-Null
Wait-LogLine 'elevator column at -?\d+ -?\d+ called to contact y' 10 -Since $mark | Out-Null

# A piston pushing a block, from the console: two poles behind, a test generator beside it.
Send-Console 'tickingarea add 0 -64 0 15 -40 15 lv_pistons_e2e true'
Wait-Seconds 3
Send-Console 'setblock 4 -60 2 levitate:piston_extension_pole ["levitate:facing"=3]'
Send-Console 'setblock 4 -60 1 levitate:piston_extension_pole ["levitate:facing"=3]'
Send-Console 'setblock 4 -60 6 gold_block'
Send-Console 'setblock 4 -60 4 stone'
Send-Console 'setblock 4 -60 3 levitate:mechanical_piston ["levitate:facing"=3,"levitate:axis_first"=1]'
$mark = Get-LogMark
Send-Console 'setblock 5 -60 3 levitate:kinetics_test_source ["levitate:axis"=0]'
Wait-LogLine 'piston at 4 -60 3 assembled: 4 blocks, offset 0' 20 -Since $mark | Out-Null
Wait-LogLine 'piston at 4 -60 3 disassembled at offset 1' 60 -Since $mark | Out-Null
Send-Console 'create kinetics 5 -60 3 speed -64'
Wait-LogLine 'piston at 4 -60 3 disassembled at offset 0' 60 -Since $mark | Out-Null
foreach ($z in 1..6) { Send-Console "setblock 4 -60 $z air" }
Send-Console 'setblock 5 -60 3 air'
Send-Console 'tickingarea remove lv_pistons_e2e'
