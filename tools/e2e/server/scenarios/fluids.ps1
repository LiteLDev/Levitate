# Fluid machines (U19): fluid registry (buckets, bottles, potions, filling / emptying recipes),
# spout (bottles, buckets, recipes, waiting for fluid, blocked position, cauldrons), item drain
# (emptying, passing items on, ejecting, player exchange), hose pulley (lowering, draining,
# filling, infinite bodies, winding up, placement facing), honey / chocolate source blocks
# (bucket place / pick up, lava, slowness) and the portable fluid interface on a contraption.
$mark = Get-LogMark
$r = Invoke-SelfTest 'fluids.*'
if ($r.Pass -lt 12) { throw "expected at least 12 fluids selftests to pass, got $($r.Pass)" }
Wait-LogLine 'spout at -?\d+ -?\d+ -?\d+ filled minecraft:glass_bottle into minecraft:potion' 10 -Since $mark | Out-Null
Wait-LogLine 'spout at -?\d+ -?\d+ -?\d+ filled a cauldron with minecraft:lava' 10 -Since $mark | Out-Null
Wait-LogLine 'item drain at -?\d+ -?\d+ -?\d+ emptied minecraft:water_bucket \(1000 mB minecraft:water\)' 10 -Since $mark | Out-Null
Wait-LogLine 'item drain at -?\d+ -?\d+ -?\d+ ejected minecraft:bucket' 10 -Since $mark | Out-Null
Wait-LogLine 'hose pulley at -?\d+ -?\d+ -?\d+ drained minecraft:water' 10 -Since $mark | Out-Null
Wait-LogLine 'hose pulley at -?\d+ -?\d+ -?\d+ placed minecraft:water' 10 -Since $mark | Out-Null
Wait-LogLine 'placed levitate:honey at' 10 -Since $mark | Out-Null
Wait-LogLine 'portable fluid interface at -?\d+ -?\d+ -?\d+ connected to contraption #\d+' 10 -Since $mark | Out-Null
Wait-LogLine 'portable fluid interface on contraption #\d+ moves on' 10 -Since $mark | Out-Null

# the blocks exist for console commands too
Send-Console 'tickingarea add 0 -64 0 15 -40 15 lv_fluids_e2e true'
Wait-Seconds 3
Send-Console 'setblock 3 -58 3 levitate:spout'
Send-Console 'setblock 4 -58 3 levitate:item_drain'
Send-Console 'setblock 5 -58 3 levitate:hose_pulley ["levitate:facing"=3]'
Send-Console 'setblock 6 -58 3 levitate:portable_fluid_interface ["levitate:facing"=1]'
Send-Console 'setblock 7 -58 3 levitate:honey'
Wait-Seconds 1
foreach ($x in 3..7) { Send-Console "setblock $x -58 3 air" }
Send-Console 'tickingarea remove lv_fluids_e2e'
