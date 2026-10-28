# Goggles (U8) on a real client:
#   local / remote-mod: wearing the goggles while looking at a running consumer shows Create's
#                       goggle panel next to the crosshair (client mod); `/create overlay` opens the
#                       position screen, a click moves the overlay, Esc saves it, `/create overlay
#                       reset` puts it back; the wrench turns the consumer; the inventory tooltip of
#                       the wrench carries Create's "Hold [Shift] for Summary".
#   remote-vanilla:     the same panel in the action bar.
# Screenshots: out/goggles/{overlay,overlay-config,overlay-moved,wrenched,tooltip}.png or overlay-actionbar.png
$vanilla = $script:Mode -eq 'remote-vanilla'
if (-not $vanilla) {
    Wait-LogLine 'companion: server accepted the handshake' 90 | Out-Null
}

Invoke-Chat '/gamemode creative'
Invoke-Chat '/clear @s'
Invoke-Chat '/time set noon'
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1.6 ~'
Start-Sleep -Milliseconds 500
Invoke-Chat '/setblock ~4 ~1 ~ levitate:kinetics_test_source ["levitate:axis"=0]'
Invoke-Chat '/setblock ~3 ~1 ~ levitate:kinetics_test_consumer ["levitate:axis"=0]'
$mark = Get-LogMark
Invoke-Chat '/replaceitem entity @s slot.armor.head 0 levitate:goggles'
Start-Sleep -Seconds 1

if ($vanilla) {
    Wait-LogLine 'goggles: overlay for .* lines, action bar\)' 30 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 800
    Save-Screenshot 'overlay-actionbar' | Out-Null
    Assert-NoErrors
    return
}

Wait-LogLine 'goggles: overlay for .* lines, client mod\)' 30 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1500
Save-Screenshot 'overlay' | Out-Null

# the overlay position screen: click moves the sample, Esc saves
$mark = Get-LogMark
Invoke-Chat '/create overlay'
Wait-LogLine 'screen opened: ' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 800
Move-Mouse 0.25 0.3 -Relative
Click 0.25 0.3 -Relative
Start-Sleep -Milliseconds 600
Save-Screenshot 'overlay-config' | Out-Null
Close-Screen
Wait-LogLine 'goggles: overlay offset set to -' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1500
Save-Screenshot 'overlay-moved' | Out-Null
$mark = Get-LogMark
Invoke-Chat '/create overlay reset'
Wait-LogLine 'goggles: overlay offset reset' 20 -Since $mark | Out-Null

# the wrench turns a consumer along z clicked on its west face (about the x axis) to y
Invoke-Chat '/setblock ~3 ~1 ~ levitate:kinetics_test_consumer ["levitate:axis"=2]'
Invoke-Chat '/give @s levitate:wrench 1'
Press-Key '1'
Start-Sleep -Milliseconds 800
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'wrench: \S+ rotated levitate:kinetics_test_consumer' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 800
Save-Screenshot 'wrenched' | Out-Null

# item tooltip: hover the wrench in the hotbar row of the survival inventory
Invoke-Chat '/gamemode survival'
Start-Sleep -Milliseconds 500
Open-Inventory
Move-Mouse 0.385 0.69 -Relative
Start-Sleep -Milliseconds 1200
Save-Screenshot 'tooltip' | Out-Null
Close-Screen
Invoke-Chat '/gamemode creative'

if ($script:Mode -eq 'local') {
    $r = Invoke-SelfTest 'goggles.*'
    if ($r.Pass -lt 9) { throw "expected at least 9 goggles selftests to pass, got $($r.Pass)" }
}
Assert-NoErrors
