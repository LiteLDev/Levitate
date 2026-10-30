# Equipment (U31) on BDS: every equipment selftest (toolbox compartments, breaking and placing
# with contents, equipping / radial protocol, dyeing, backtank filling / wearing / air tools,
# diving helmet and the netherite set, potato cannon planting and damage, the potato projectile
# entity (variants, sticky ammo, collisions, recovery, golden apples), Capacity / Potato Recovery
# (backtank air, the enchanting table, anvil and grindstone), wand of symmetry,
# worldshaper, sand paper, tree fertilizer, linked controller and lectern), then blocks set from
# the console.
$r = Invoke-SelfTest 'equipment.*'
if ($r.Pass -lt 26) { throw "expected at least 26 equipment selftests to pass, got $($r.Pass)" }
Wait-LogLine 'equipment: .* equipped minecraft:torch from the toolbox at ' 10 | Out-Null
Wait-LogLine 'equipment: .* fired a potato cannon \(potato\)' 10 | Out-Null
Wait-LogLine 'equipment: a potato cannon planted potato at ' 10 | Out-Null
Wait-LogLine 'equipment: a cake projectile stuck to minecraft:cow' 10 | Out-Null
Wait-LogLine 'equipment: potato projectiles collided \((potato and carrot|carrot and potato)\)' 10 | Out-Null
Wait-LogLine 'equipment: a potato cannon cured the zombie villager at ' 10 | Out-Null
Wait-LogLine 'equipment: .* placed a symmetry mirror|equipment: .* used the worldshaper \(Cuboid / Fill\)' 10 | Out-Null
Wait-LogLine 'equipment: toolbox at .* dyed lime by ' 10 | Out-Null
Wait-LogLine 'equipment: .* used tree fertilizer on minecraft:oak_sapling at .* \(grew: dream world ' 10 | Out-Null

# a toolbox, a backtank on a turning shaft and a lectern controller from the console
Send-Console 'tickingarea add 0 -64 0 15 0 15 lv_equipment'
Wait-Seconds 3
$mark = Get-LogMark
Send-Console 'setblock 4 -60 4 levitate:white_toolbox ["levitate:facing"=2]'
Send-Console 'setblock 6 -61 4 levitate:kinetics_test_source ["levitate:axis"=1]'
Send-Console 'setblock 6 -60 4 levitate:copper_backtank ["levitate:facing"=0]'
Wait-Seconds 1
Send-Console 'create kinetics 6 -61 4 speed 256'
Wait-LogLine 'equipment: backtank at 6 -60 4 is full' 30 -Since $mark | Out-Null
Send-Console 'fill 4 -61 4 6 -60 4 air'
Send-Console 'tickingarea remove lv_equipment'
