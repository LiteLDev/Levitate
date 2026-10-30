# U13 bulk processing: millstone, crushing wheels (+ controller), encased fan (current, pushing,
# washing / smoking / blasting / haunting), nozzle, placement rules; then a fan driven from the
# console with its log line.
$r = Invoke-SelfTest 'bulk.*'
if ($r.Pass -lt 22) { throw "expected at least 22 bulk selftests to pass, got $($r.Pass)" }
# U39: a crushed pig, an overloaded pulling nozzle, a refused nozzle placement
Wait-LogLine 'crushing wheels at .*: crushed minecraft:pig' 10 | Out-Null
Wait-LogLine 'nozzle at .*: 260 entities, exploded' 10 | Out-Null
Wait-LogLine "levitate:nozzle at .*: can't be placed there \(canSurvive\)" 10 | Out-Null
Wait-LogLine 'millstone at .*: milled minecraft:bone into \d+ item' 10 | Out-Null
Wait-LogLine 'crushing wheels at .*: crushed minecraft:cobblestone x2 into 2 item' 10 | Out-Null
Wait-LogLine 'encased fan at .*: splashing turned minecraft:red_wool x2 into minecraft:white_wool x2' 10 | Out-Null
Wait-LogLine 'encased fan at .*: smoking turned minecraft:beef x1 into minecraft:cooked_beef x1' 10 | Out-Null
Wait-LogLine 'nozzle at .*: range' 10 | Out-Null

# A fan from the console: a test generator behind an upward fan.
Send-Console 'tickingarea add 0 -64 0 15 -40 15 lv_bulk_e2e true'
Wait-Seconds 3
Send-Console 'setblock 6 -58 6 levitate:kinetics_test_source ["levitate:axis"=1]'
Send-Console 'setblock 6 -57 6 levitate:encased_fan ["levitate:facing"=1]'
Wait-Seconds 1
$mark = Get-LogMark
Send-Console 'create kinetics 6 -58 6 speed 256'
Wait-LogLine 'encased fan at 6 -57 6: (pushing|pulling) 20\.00 blocks' 20 -Since $mark | Out-Null
Send-Console 'setblock 6 -57 6 air'
Send-Console 'setblock 6 -58 6 air'
Send-Console 'tickingarea remove lv_bulk_e2e'
