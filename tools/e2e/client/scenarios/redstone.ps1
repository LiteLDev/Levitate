# Redstone (U23) on a real client: the block models in their states, a lever-driven link pair
# lighting a lamp, the threshold switch screen (native with the client mod, a form for vanilla
# clients) and the link hover panel (client mod).
# Screenshots: out/redstone/{blocks,threshold-screen,link-hover}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

if (-not $vanilla) {
    $r = Invoke-SelfTest 'redstone.*'
    if ($r.Pass -lt 18) { throw "expected at least 18 redstone selftests to pass, got $($r.Pass)" }
}

Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1.6 ~'
Start-Sleep -Milliseconds 500
# a row of blocks 4..6 blocks ahead (east), from left (north) to right (south)
Invoke-Chat '/setblock ~5 ~ ~-3 levitate:analog_lever ["levitate:face"=0,"levitate:facing"=2,"levitate:state"=12]'
Invoke-Chat '/setblock ~5 ~ ~-2 levitate:redstone_link ["levitate:facing"=1]'
Invoke-Chat '/setblock ~6 ~ ~-2 levitate:redstone_link ["levitate:facing"=1,"levitate:receiver"=1]'
Invoke-Chat '/setblock ~7 ~ ~-2 redstone_lamp'
Invoke-Chat '/setblock ~5 ~ ~-1 levitate:pulse_extender ["levitate:facing"=4,"levitate:inverted"=1]'
Invoke-Chat '/setblock ~5 ~ ~1 levitate:powered_latch ["levitate:facing"=4,"levitate:powering"=1]'
Invoke-Chat '/setblock ~5 ~ ~2 levitate:pulse_timer ["levitate:facing"=4]'
Invoke-Chat '/setblock ~5 ~ ~3 levitate:powered_toggle_latch ["levitate:facing"=4]'
Invoke-Chat '/setblock ~6 ~1 ~2 levitate:redstone_contact ["levitate:facing"=3]'
Invoke-Chat '/setblock ~6 ~1 ~3 levitate:redstone_contact ["levitate:facing"=2]'
Invoke-Chat '/setblock ~6 ~1 ~-1 levitate:content_observer ["levitate:facing"=5]'
Invoke-Chat '/setblock ~4 ~1 ~ chest'
Invoke-Chat '/setblock ~3 ~1 ~ levitate:stockpile_switch ["levitate:facing"=5]'
Start-Sleep -Seconds 3
$mark = Get-LogMark
Save-Screenshot 'blocks' | Out-Null

# the threshold switch: right-click its back face away from the filter slot in the middle
Invoke-Chat '/tp @s ~ ~ ~ facing ~2.5 ~-0.12 ~-0.3'
# an item in hand: Create opens the screen with anything but the wrench, and the game only
# sends a block use for an empty hand on blocks it knows to be interactive
Invoke-Chat '/give @s stick'
Start-Sleep -Milliseconds 800
Click 0.5 0.5 -Relative -Right
if ($vanilla) {
    Wait-LogLine 'screen redstone.threshold_switch opened for .* \(form\)' 20 -Since $mark | Out-Null
} else {
    Wait-LogLine 'screen redstone.threshold_switch opened for .* \(native\)' 20 -Since $mark | Out-Null
}
Start-Sleep -Milliseconds 1200
Save-Screenshot 'threshold-screen' | Out-Null
Close-Screen

# the receiver link: hover shows its frequencies (client mod)
Invoke-Chat '/tp @s ~ ~ ~ facing ~6 ~-1.5 ~-2'
Start-Sleep -Seconds 2
Save-Screenshot 'link-hover' | Out-Null
Assert-NoErrors
