# Crafter unit (U16): every crafter / saw / drill selftest (geometry, recipes, grid crafting,
# redstone start, connected inputs, slot cover, hand use, cutting, recipe filter, tree felling,
# drill breaking, drill on a contraption), then a powered drill set up from the console.
$r = Invoke-SelfTest 'crafter.*'
if ($r.Pass -lt 12) { throw "expected at least 12 crafter selftests to pass, got $($r.Pass)" }

# A drill fed by a test source breaks the stone in front of it (log line per break).
Send-Console 'tickingarea add 0 -64 0 15 -40 15 lv_crafter_e2e true'
Wait-Seconds 3
Send-Console 'setblock 4 -58 4 levitate:kinetics_test_source ["levitate:axis"=0]'
Send-Console 'setblock 5 -58 4 levitate:mechanical_drill ["levitate:facing"=5]'
Send-Console 'setblock 6 -58 4 stone'
Wait-Seconds 1
$mark = Get-LogMark
Send-Console 'create kinetics 4 -58 4 speed 64'
Wait-LogLine 'drill at 0:5:-58:4 broke minecraft:stone at 0:6:-58:4' 20 -Since $mark | Out-Null
Send-Console 'setblock 4 -58 4 air'
Send-Console 'setblock 5 -58 4 air'
Send-Console 'kill @e[type=item]'
Send-Console 'tickingarea remove lv_crafter_e2e'
