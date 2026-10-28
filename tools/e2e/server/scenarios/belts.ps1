# Belts (U11) on BDS: the selftests on a real world (connecting, rules, transport, queueing,
# belt-to-belt transfer, entities, breaking, editing, depots, processing, ejectors, persistence),
# then a belt built with commands that carries a stack into a depot.
$r = Invoke-SelfTest 'belts.*'
if ($r.Pass -lt 12) { throw "expected at least 12 belts selftests to pass, got $($r.Pass)" }
Wait-LogLine 'belts: lv_test_\d+ connected a belt from .* \(5 blocks\)' 10 | Out-Null
Wait-LogLine 'belts: belt broken at .* \(5 blocks\)' 10 | Out-Null
Wait-LogLine 'belts: ejector at .* launched minecraft:redstone x4' 10 | Out-Null

# A belt laid with /setblock (start, middle, end; facing east) gets its metadata and turns.
Send-Console 'tickingarea add 0 -64 0 15 0 15 lv_belts'
Wait-Seconds 3
$mark = Get-LogMark
Send-Console 'setblock 2 -58 1 levitate:kinetics_test_source ["levitate:axis"=2]'
Send-Console 'setblock 2 -58 2 levitate:belt ["levitate:facing"=3,"levitate:part"=0]'
Send-Console 'setblock 3 -58 2 levitate:belt ["levitate:facing"=3,"levitate:part"=1]'
Send-Console 'setblock 4 -58 2 levitate:belt ["levitate:facing"=3,"levitate:part"=2]'
Wait-Seconds 1
Send-Console 'create kinetics 4 -58 2'
Wait-LogLine 'kinetics: Speed: 16 RPM' 15 -Since $mark | Out-Null
Send-Console 'tickingarea remove lv_belts'