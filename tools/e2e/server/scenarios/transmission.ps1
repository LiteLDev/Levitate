# Transmission (U6) on BDS: every transmission selftest (shafts, cogs, gearbox, clutch / gearshift,
# chain drives, speed controller, sequenced gearshift, gauges, placement rules, the sequencer
# screen), then a line built with /setblock: a clutch powered by a redstone block from the console.
$r = Invoke-SelfTest 'transmission.*'
if ($r.Pass -lt 10) { throw "expected at least 10 transmission selftests to pass, got $($r.Pass)" }
Wait-LogLine 'transmission: sequenced gearshift at .* runs instruction 1 \(turn_angle\)' 10 | Out-Null
Wait-LogLine 'transmission: speed controller at .* targets -48 RPM' 10 | Out-Null
Wait-LogLine 'transmission: .* placed levitate:shaft at .* by placement assist' 10 | Out-Null

Send-Console 'tickingarea add 0 -64 0 15 0 15 lv_transmission'
Wait-Seconds 3
$mark = Get-LogMark
Send-Console 'setblock 2 -58 6 levitate:kinetics_test_source ["levitate:axis"=0]'
Send-Console 'setblock 3 -58 6 levitate:clutch ["levitate:axis"=0]'
Send-Console 'setblock 4 -58 6 levitate:shaft ["levitate:axis"=0]'
Wait-Seconds 1
Send-Console 'create kinetics 4 -58 6'
Wait-LogLine 'kinetics: Speed: 16 RPM' 15 -Since $mark | Out-Null
Send-Console 'setblock 3 -57 6 redstone_block'
Wait-LogLine 'transmission: levitate:clutch at .* powered' 15 -Since $mark | Out-Null
Wait-Seconds 1
$mark2 = Get-LogMark
Send-Console 'create kinetics 4 -58 6'
Wait-LogLine 'kinetics: Speed: 0 RPM' 15 -Since $mark2 | Out-Null
Send-Console 'setblock 3 -57 6 air'
Wait-LogLine 'transmission: levitate:clutch at .* unpowered' 15 -Since $mark | Out-Null
Send-Console 'tickingarea remove lv_transmission'
