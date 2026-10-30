# U42 on a real client: Create's click and hold on value boxes, the box outline, the config screen,
# the gauge panels, the overstressed speed text, the wrench in hand and the missing-room outline.
#   local / remote-mod: a valve handle's value box: outline + "Click and hold to edit" label
#                       (valve-box.png), a short click turns the handle, holding opens the board
#                       (valve-board.png); /create config kinetics (config.png); two connected
#                       factory gauges (gauges.png); a speedometer of an overstressed network seen
#                       with goggles (overstressed.png); the wrench in hand (wrench.png); a blocked
#                       large water wheel (room.png).
#   remote-vanilla:     the click on the box opens the form at once (valve-form.png); the config
#                       form (config-form.png).
$vanilla = $script:Mode -eq 'remote-vanilla'
if (-not $vanilla) { Wait-LogLine 'companion joined: ' 90 | Out-Null }

Invoke-Chat '/gamemode creative'
Invoke-Chat '/clear @s'
Invoke-Chat '/fill 1 -60 -2 9 -57 6 air'
# a copper valve handle on the ground, its value box on top
Invoke-Chat '/setblock 2 -60 0 levitate:copper_valve_handle ["levitate:facing"=1]'
Invoke-Chat '/tp @s 2.5 -60 -0.6 facing 2.5 -59.72 0.5'
Start-Sleep -Milliseconds 800

if ($vanilla) {
    $mark = Get-LogMark
    Click 0.5 0.5 -Relative -Right
    Wait-LogLine 'valve handle at .* turned' 20 -Since $mark | Out-Null
    Invoke-Chat '/give @s levitate:wrench'
    Start-Sleep -Milliseconds 600
    $mark = Get-LogMark
    Click 0.5 0.5 -Relative -Right
    Wait-LogLine 'value box angle at .* opened for .* \(form\)' 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 1200
    Save-Screenshot 'valve-form' | Out-Null
    Close-Screen
    $mark = Get-LogMark
    Invoke-Chat '/create config'
    Wait-LogLine 'config screen \(sections\) opened for' 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 1200
    Save-Screenshot 'config-form' | Out-Null
    Close-Screen
    Assert-NoErrors
    return
}

Wait-LogLine 'valuebox client: hover label .*Click and hold to edit' 20 | Out-Null
Start-Sleep -Milliseconds 600
Save-Screenshot 'valve-box' | Out-Null

# a short click: the handle turns (the box's short click)
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'value box hold at .*: short' 20 -Since $mark | Out-Null
Wait-LogLine 'valve handle at .* turned' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1500

# holding: the angle board opens
$mark = Get-LogMark
[BsE2E.Native]::MouseButton(1, $false)
Start-Sleep -Milliseconds 900
[BsE2E.Native]::MouseButton(1, $true)
Wait-LogLine 'value box hold at .*: hold' 20 -Since $mark | Out-Null
Wait-LogLine 'value box angle at .* opened for .* \(native\)' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1200
Save-Screenshot 'valve-board' | Out-Null
Close-Screen

# the config screen
$mark = Get-LogMark
Invoke-Chat '/create config kinetics'
Wait-LogLine 'config client: screen kinetics opened' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1200
Save-Screenshot 'config' | Out-Null
Close-Screen

# two connected factory gauges on the ground east of the player
Invoke-Chat '/tp @s 2.5 -60 3.5 -90 0'
Start-Sleep -Milliseconds 400
$mark = Get-LogMark
Invoke-Chat '/create stock gauges'
Wait-LogLine 'stock: gauge demo built at' 20 -Since $mark | Out-Null
Invoke-Chat '/tp @s 4.5 -59 2.2 facing 5 -60 3.5'
Wait-LogLine 'stock client: drawing \d+ gauge panels' 30 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1500
Save-Screenshot 'gauges' | Out-Null

# an overstressed network: the test source (128 su) against three test consumers (192 su), a
# speedometer in it, seen with goggles: the speed text is struck through
Invoke-Chat '/replaceitem entity @s slot.armor.head 0 levitate:goggles'
Invoke-Chat '/setblock 3 -60 5 levitate:kinetics_test_source ["levitate:axis"=0]'
Invoke-Chat '/setblock 4 -60 5 levitate:speedometer ["levitate:facing"=2,"levitate:along_first"=1]'
Invoke-Chat '/fill 5 -60 5 7 -60 5 levitate:kinetics_test_consumer ["levitate:axis"=0]'
Invoke-Chat '/tp @s 4.5 -60 3.6 facing 4.5 -59.5 5.5'
Start-Sleep -Milliseconds 2500
Save-Screenshot 'overstressed' | Out-Null
Invoke-Chat '/replaceitem entity @s slot.armor.head 0 air'

# the wrench in hand (its cog turns)
Invoke-Chat '/clear @s'
Invoke-Chat '/give @s levitate:wrench'
Press-Key '1'
Invoke-Chat '/tp @s 2.5 -60 -3 0 0'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'wrench' | Out-Null

# a large water wheel without room: Create's red outline of the 3x3 it needs
Invoke-Chat '/clear @s'
Invoke-Chat '/give @s levitate:large_water_wheel'
Press-Key '1'
# the wheel would go in front of the wall (8 -60 -1), but stone fills its plane around it
Invoke-Chat '/fill 7 -60 0 9 -59 0 stone'
Invoke-Chat '/setblock 7 -60 -1 stone'
Invoke-Chat '/setblock 9 -60 -1 stone'
Invoke-Chat '/setblock 8 -59 -1 stone'
Invoke-Chat '/tp @s 8.5 -60 -3.5 facing 8.5 -59.5 0'
Start-Sleep -Milliseconds 800
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'generators client: missing room outlined' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 200
Save-Screenshot 'room' | Out-Null

Invoke-Chat '/fill 1 -60 -2 9 -57 6 air'
if ($script:Mode -eq 'local') {
    $r = Invoke-SelfTest 'common.value_box*'
    if ($r.Fail -gt 0) { throw "value box selftests failed" }
}
Assert-NoErrors
