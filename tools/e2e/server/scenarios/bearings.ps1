# Rotating contraptions (U21): every bearings selftest (mechanical bearing modes, windmill sails and
# generation, clockwork hands, turntable, sails, cart assembler, controller rail, couplings, minecart
# contraption pickup, the coupling sync for the client mod's coupling renderer), then a bearing driven from the console with its log lines.
$r = Invoke-SelfTest 'bearings.*'
if ($r.Pass -lt 15) { throw "expected at least 15 bearings selftests to pass, got $($r.Pass)" }

# A mechanical bearing with a block on top, driven by the kinetic test source from the console.
Send-Console 'tickingarea add 0 -64 0 15 -40 15 lv_bearings_e2e true'
Wait-Seconds 3
Send-Console 'setblock 6 -60 6 levitate:kinetics_test_source ["levitate:axis"=1]'
Send-Console 'setblock 6 -59 6 levitate:mechanical_bearing ["levitate:facing"=1]'
Send-Console 'setblock 6 -58 6 oak_planks'
Wait-Seconds 1
$mark = Get-LogMark
Send-Console 'create kinetics 6 -60 6 speed 32'
$line = Wait-LogLine 'mechanical_bearing at .* assembled contraption #(\d+)' 20 -Since $mark
$id = [regex]::Match($line, 'contraption #(\d+)').Groups[1].Value
Wait-Seconds 2
Send-Console 'create contraption list'
Send-Console 'create kinetics 6 -60 6 speed 0'
Wait-LogLine 'mechanical_bearing at .* disassembled' 20 -Since $mark | Out-Null
Wait-LogLine "contraption #$id disassembled" 20 -Since $mark | Out-Null
Send-Console 'setblock 6 -58 6 air'
Send-Console 'setblock 6 -59 6 air'
Send-Console 'setblock 6 -60 6 air'
Send-Console 'tickingarea remove lv_bearings_e2e'
