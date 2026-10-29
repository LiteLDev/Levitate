# Generators (U7) on BDS: every generators selftest (registry, hand crank, clicks through
# GameMode, placement orientation, valve handle angles / dyeing, value boxes, creative motor,
# water wheels driven by bubble columns and by flowing water, the large water wheel's structure,
# wood and placement), their log lines, then a creative motor built from the console.
$r = Invoke-SelfTest 'generators.*'
if ($r.Pass -lt 11) { throw "expected at least 11 generators selftests to pass, got $($r.Pass)" }
Wait-LogLine 'hand crank at .* turned by' 10 | Out-Null
Wait-LogLine 'valve handle at .* turned 45 degrees by' 10 | Out-Null
Wait-LogLine 'valve handle at .* dyed red by' 10 | Out-Null
Wait-LogLine 'levitate:hand_crank placed at .* facing up by' 10 | Out-Null
Wait-LogLine 'water wheel at .*: flow score -1, -8 RPM' 10 | Out-Null
Wait-LogLine 'large water wheel at .*: flow score -3, -4 RPM' 10 | Out-Null
Wait-LogLine 'water wheel at .* now uses birch wood' 10 | Out-Null
Wait-LogLine 'large water wheel at .* broke with its structure at' 10 | Out-Null
Wait-LogLine 'large water wheel placed at .* by' 10 | Out-Null
Wait-LogLine 'value box speed at .* set to 200' 10 | Out-Null

# A creative motor facing east driving two shafts, kept loaded by a ticking area.
Send-Console 'tickingarea add 0 -64 0 15 0 15 lv_generators'
Wait-Seconds 3
$mark = Get-LogMark
Send-Console 'setblock 2 -58 6 levitate:creative_motor ["levitate:facing"=5]'
Send-Console 'setblock 3 -58 6 levitate:kinetics_test_shaft ["levitate:axis"=0]'
Send-Console 'setblock 4 -58 6 levitate:kinetics_test_shaft ["levitate:axis"=0]'
Wait-Seconds 1
Send-Console 'create kinetics 4 -58 6'
Wait-LogLine 'kinetics: Speed: 16 RPM' 15 -Since $mark | Out-Null
# a large water wheel from the console builds its structure
Send-Console 'setblock 8 -56 8 levitate:large_water_wheel ["levitate:axis"=0]'
Wait-Seconds 1
Send-Console 'testforblock 8 -55 9 levitate:water_wheel_structure'
Wait-LogLine 'Successfully found the block' 15 -Since $mark | Out-Null
Send-Console 'setblock 8 -56 8 air'
Send-Console 'setblock 2 -58 6 air'
Send-Console 'tickingarea remove lv_generators'
