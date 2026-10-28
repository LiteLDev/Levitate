# Assembly (U15) on the client: a deployer and a mechanical arm in front of the player.
#   local / remote-mod: the deployer gets cobblestone by a right-click on its front face and places
#                       it (hand moving: client mod animation), the arm rests drawn by the client
#                       mod; the selftests pass on the (integrated) server.
#   remote-vanilla:     the static models, without errors.
# Screenshots: out/assembly/{placed-blocks,deployer-holding,deployer-working,deployer-done}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

if (-not $vanilla) {
    $r = Invoke-SelfTest 'assembly.*'
    if ($r.Pass -lt 11) { throw "expected at least 11 assembly selftests to pass, got $($r.Pass)" }
}

Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
# Look along +x: the deployer 3 blocks ahead faces the player (west), its shaft along z turned by a
# test source behind it; an arm on the floor to the right, a composter in front of it.
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1.6 ~'
Start-Sleep -Milliseconds 500
Invoke-Chat '/setblock ~3 ~1 ~ levitate:deployer ["levitate:facing"=4,"levitate:axis_along_first"=0]'
Invoke-Chat '/setblock ~3 ~1 ~-1 levitate:kinetics_test_source ["levitate:axis"=2]'
Invoke-Chat '/setblock ~4 ~ ~2 levitate:mechanical_arm'
Invoke-Chat '/setblock ~4 ~-1 ~2 levitate:kinetics_test_source ["levitate:axis"=1]'
Invoke-Chat '/setblock ~3 ~ ~3 minecraft:composter'
Start-Sleep -Seconds 2
Save-Screenshot 'placed-blocks' | Out-Null

# face the deployer's front and hand it a stack of cobblestone
Invoke-Chat '/clear @s'
Invoke-Chat '/give @s cobblestone 16'
Invoke-Chat '/tp @s ~ ~ ~ facing ~3 ~1.5 ~0.5'
Start-Sleep -Milliseconds 600
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'deployer at .* now holds minecraft:cobblestone' 20 -Since $mark | Out-Null
Save-Screenshot 'deployer-holding' | Out-Null
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1.6 ~'
if (-not $vanilla) {
    Wait-LogLine 'assembly client: drawing \d+ deployer / arm part set' 30 | Out-Null
}
Start-Sleep -Milliseconds 700
Save-Screenshot 'deployer-working' | Out-Null
Wait-LogLine 'deployer at .* used minecraft:cobblestone' 30 -Since $mark | Out-Null
Start-Sleep -Seconds 1
Save-Screenshot 'deployer-done' | Out-Null
Assert-NoErrors
