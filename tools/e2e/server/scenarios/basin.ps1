# Basin unit (U14): every basin selftest (inventory, tanks, player use, item intake, blaze burners,
# capturing, mixing, heat, compacting, filters, pressing in the world, spouts, placement, sync),
# then a press driven from the console pressing an item dropped under it.
$r = Invoke-SelfTest 'basin.*'
if ($r.Pass -lt 15) { throw "expected at least 15 basin selftests to pass, got $($r.Pass)" }

Send-Console 'tickingarea add 0 -64 0 15 -40 15 lv_basin_e2e true'
Wait-Seconds 3
# floor, a press two blocks above it turned by the kinetic test source, a sugar cane to drop
Send-Console 'setblock 6 -61 6 stone'
Send-Console 'setblock 6 -59 6 levitate:mechanical_press ["levitate:axis"=0]'
Send-Console 'setblock 7 -59 6 levitate:kinetics_test_source ["levitate:axis"=0]'
Send-Console 'setblock 9 -61 6 sugar_cane'
Send-Console 'setblock 9 -62 6 sand'
Send-Console 'setblock 10 -62 6 water'
Wait-Seconds 1
Send-Console 'create kinetics 7 -59 6 speed 128'
$mark = Get-LogMark
Send-Console 'loot spawn 6 -60 6 mine 9 -61 6'
Wait-LogLine 'press at .* pressed minecraft:sugar_cane' 30 -Since $mark | Out-Null

# a basin collects items dropped into it
Send-Console 'setblock 3 -61 3 levitate:basin'
Send-Console 'setblock 9 -61 6 sugar_cane'
Wait-Seconds 1
$mark = Get-LogMark
Send-Console 'loot spawn 3 -59 3 mine 9 -61 6'
Wait-LogLine 'basin at .* took minecraft:sugar_cane' 30 -Since $mark | Out-Null

foreach ($c in @('6 -61 6', '6 -59 6', '7 -59 6', '9 -61 6', '9 -62 6', '10 -62 6', '3 -61 3')) { Send-Console "setblock $c air" }
Send-Console 'kill @e[type=item]'
Send-Console 'tickingarea remove lv_basin_e2e'
