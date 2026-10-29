# Steam (U18) on BDS: the selftests (tank multiblocks, fluids, buckets, windows, creative tanks,
# boilers driving engines, passive boilers, engine / whistle support, whistle pitch and sound),
# then a boiler built with /setblock and inspected with `/create steam`.
$mark = Get-LogMark
$r = Invoke-SelfTest 'steam.*'
if ($r.Pass -lt 12) { throw "expected at least 12 steam selftests to pass, got $($r.Pass)" }
Wait-LogLine 'steam: tank at .* is now a boiler \(1 engines, 0 whistles\)' 10 -Since $mark | Out-Null
Wait-LogLine 'steam: powered shaft at .* turns at -64 RPM \(16,384 su\)' 10 -Since $mark | Out-Null
Wait-LogLine 'steam: boiler at .* level passive' 10 -Since $mark | Out-Null
Wait-LogLine 'steam: whistle at .* sounds \(medium whistle, pitch 3\)' 10 -Since $mark | Out-Null

# A 1x1x4 tank on a kindled test heater with an engine on top, next to the spawn, kept loaded.
Send-Console 'tickingarea add 0 -64 0 15 0 15 lv_steam'
Wait-Seconds 3
$mark = Get-LogMark
Send-Console 'setblock 8 -61 8 levitate:steam_test_heater ["levitate:heat"=3]'
foreach ($y in -60..-57) { Send-Console "setblock 8 $y 8 levitate:fluid_tank" }
Wait-Seconds 1
Wait-LogLine 'steam: levitate:fluid_tank placed at 8 -57 8 \(1x4x1 tank at 8 -60 8\)' 15 -Since $mark | Out-Null
Send-Console 'create steam 8 -60 8 fill water 3000'
Wait-LogLine 'steam: 3000 mB of minecraft:water moved at 8 -60 8' 15 -Since $mark | Out-Null
Send-Console 'create steam 8 -58 8'
Wait-LogLine 'steam: Fluid Container Info' 15 -Since $mark | Out-Null
Wait-LogLine 'steam: .*3,000mB.* / .*32,000mB' 15 -Since $mark | Out-Null
Send-Console 'setblock 8 -56 8 levitate:steam_engine ["minecraft:block_face"="up"]'
Wait-LogLine 'steam: steam engine placed at 8 -56 8 facing up' 15 -Since $mark | Out-Null
Wait-LogLine 'steam: tank at 8 -60 8 is now a boiler' 15 -Since $mark | Out-Null
Send-Console 'create steam 8 -56 8'
Wait-LogLine 'steam: Boiler Status: .*Idle' 15 -Since $mark | Out-Null
Send-Console 'setblock 8 -61 8 grass'
Send-Console 'tickingarea remove lv_steam'
