# Kinetic network (U2) on BDS: the selftests on a real world (propagation, cogs, breaking, conflicts,
# overstress, state changes, persistence, listeners, sync payloads), then the console command on a
# line built with /setblock.
$r = Invoke-SelfTest 'kinetics.*'
if ($r.Pass -lt 10) { throw "expected at least 10 kinetics selftests to pass, got $($r.Pass)" }
Wait-LogLine 'kinetics: levitate:kinetics_test_shaft at .* broke \(speed conflict\)' 10 | Out-Null
Wait-LogLine 'kinetics: network .* overstressed \(192 / 128 su\)' 10 | Out-Null
Wait-LogLine 'kinetics: network .* no longer overstressed' 10 | Out-Null

# A source and two shafts along x next to the spawn, kept loaded by a ticking area.
Send-Console 'tickingarea add 0 -64 0 15 0 15 lv_kinetics'
Wait-Seconds 3
$mark = Get-LogMark
Send-Console 'setblock 2 -58 2 levitate:kinetics_test_source ["levitate:axis"=0]'
Send-Console 'setblock 3 -58 2 levitate:kinetics_test_shaft ["levitate:axis"=0]'
Send-Console 'setblock 4 -58 2 levitate:kinetics_test_shaft ["levitate:axis"=0]'
Wait-Seconds 1
Send-Console 'create kinetics 4 -58 2'
Wait-LogLine 'kinetics: Speed: 16 RPM' 15 -Since $mark | Out-Null
Wait-LogLine 'kinetics: Network 2 -58 2: 3 blocks, 1 sources' 15 -Since $mark | Out-Null
Send-Console 'create kinetics 2 -58 2 speed -64'
Wait-LogLine 'kinetics: Generator at 2 -58 2 now turns at -64 RPM' 15 -Since $mark | Out-Null
Send-Console 'create kinetics 4 -58 2'
Wait-LogLine 'kinetics: Speed: -64 RPM' 15 -Since $mark | Out-Null
Send-Console 'tickingarea remove lv_kinetics'