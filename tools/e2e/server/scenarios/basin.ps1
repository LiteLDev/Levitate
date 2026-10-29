# Basin unit (U14): every basin selftest (inventory, tanks, player use, item intake, blaze burners,
# capturing, mixing, heat, compacting, filters, pressing in the world, spouts, placement, sync),
# then a press driven from the console pressing an item dropped under it and a basin collecting one.
$r = Invoke-SelfTest 'basin.*'
if ($r.Pass -lt 15) { throw "expected at least 15 basin selftests to pass, got $($r.Pass)" }

Send-Console 'tickingarea add 0 -64 0 15 -40 15 lv_basin_e2e true'
Wait-Seconds 3
# a dirt block to take an item from (loot spawn ... mine)
Send-Console 'setblock 9 -61 6 dirt'
# floor, glass walls so the drop stays put, a press two blocks above turned by the kinetic test source
Send-Console 'setblock 6 -61 6 stone'
foreach ($w in @('5 -60 6', '7 -60 6', '6 -60 5', '6 -60 7')) { Send-Console "setblock $w glass" }
Send-Console 'setblock 6 -59 6 levitate:mechanical_press ["levitate:axis"=0]'
Send-Console 'setblock 7 -59 6 levitate:kinetics_test_source ["levitate:axis"=0]'
Wait-Seconds 1
Send-Console 'create kinetics 7 -59 6 speed 128'
Wait-Seconds 1
$mark = Get-LogMark
Send-Console 'loot spawn 6 -60 6 mine 9 -61 6'
Wait-LogLine 'press at .* pressed minecraft:dirt' 30 -Since $mark | Out-Null

# a basin collects items dropped into it
Send-Console 'setblock 3 -61 3 levitate:basin'
foreach ($w in @('2 -60 3', '4 -60 3', '3 -60 2', '3 -60 4', '2 -59 3', '4 -59 3', '3 -59 2', '3 -59 4')) { Send-Console "setblock $w glass" }
Wait-Seconds 1
$mark = Get-LogMark
Send-Console 'loot spawn 3 -59 3 mine 9 -61 6'
Wait-LogLine 'basin at .* took minecraft:dirt' 30 -Since $mark | Out-Null

foreach ($c in @('6 -61 6', '5 -60 6', '7 -60 6', '6 -60 5', '6 -60 7', '6 -59 6', '7 -59 6', '9 -61 6', '3 -61 3', '2 -60 3', '4 -60 3', '3 -60 2', '3 -60 4', '2 -59 3', '4 -59 3', '3 -59 2', '3 -59 4')) { Send-Console "setblock $c air" }
Send-Console 'kill @e[type=item]'
Send-Console 'tickingarea remove lv_basin_e2e'
