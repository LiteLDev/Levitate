# Display unit (U24) on a real client:
#   a turning 3x2 display board (driven by U2's test generator) showing text written from chat,
#   a row of nixie tubes, the display link screen and the clipboard screen.
#   local / remote-mod: the client mod draws the text (split flaps, nixie glyphs), native screens.
#   remote-vanilla:     name-tag text and server forms.
# Screenshots: out/display/{board,link-screen|link-form,clipboard-tip,clipboard-screen|clipboard-form}.png
$vanilla = $script:Mode -eq 'remote-vanilla'
if (-not $vanilla) {
    Wait-LogLine 'companion: server accepted the handshake' 90 | Out-Null
}
Reset-CameraPerspective
Invoke-Chat '/clear @s'
Invoke-Chat '/tp @s 0 -60 0 facing 0 -59 6'
Start-Sleep -Milliseconds 800

# a 3x2 board facing north (towards the player) with a generator behind it
foreach ($x in -1, 0, 1) {
    foreach ($y in -60, -59) { Invoke-Chat "/setblock $x $y 5 levitate:display_board [""levitate:facing""=2]" }
}
Invoke-Chat '/setblock 0 -60 6 levitate:kinetics_test_source ["levitate:axis"=2]'
Start-Sleep -Milliseconds 600
$mark = Get-LogMark
Invoke-Chat '/create kinetics 0 -60 6 speed 64'
# nixie tubes on the floor, the row running west to east (facing south rotated: see SPEC §3.6.24)
Invoke-Chat '/setblock -3 -60 3 levitate:nixie_tube ["levitate:facing"=3]'
Invoke-Chat '/setblock -2 -60 3 levitate:nixie_tube ["levitate:facing"=3]'
Start-Sleep -Milliseconds 600
# text: the board's controller is its top east end (the left end as seen from the front)
Invoke-Chat '/create display text 1 -59 5 0 HELLO'
Wait-LogLine "display text 'HELLO' written at 0:1:-59:5" 20 -Since $mark | Out-Null
Invoke-Chat '/create display text 1 -59 5 2 CREATE_6'
Invoke-Chat '/create display text -3 -60 3 0 LV24'
Start-Sleep -Milliseconds 4000
Save-Screenshot 'board' | Out-Null

# a display link on top of the right nixie tube, bound to the board
Invoke-Chat '/setblock -2 -59 3 levitate:display_link ["minecraft:block_face"="up"]'
Start-Sleep -Milliseconds 500
$mark = Get-LogMark
Invoke-Chat '/create display bind -2 -59 3 1 -59 5 nixie_tube'
Wait-LogLine 'display link at 0:-2:-59:3 bound to 1 -59 5 \(nixie_tube\), update sent' 20 -Since $mark | Out-Null
Invoke-Chat '/tp @s -2 -60 1 0 12'
Start-Sleep -Milliseconds 800
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
if ($vanilla) {
    Wait-LogLine 'screen display\.link opened for .* \(form\)' 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 1500
    Save-Screenshot 'link-form' | Out-Null
} else {
    Wait-LogLine 'screen display\.link opened for .* \(native\)' 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 1200
    Save-Screenshot 'link-screen' | Out-Null
}
# the inventory key closes native screens (and forms do not mind a second key)
if ($vanilla) { Close-Screen } else { Press-Key E; Start-Sleep -Milliseconds 800 }

if ($script:Mode -eq 'local') {
    $r = Invoke-SelfTest 'display.*'
    if ($r.Pass -lt 11) { throw "expected at least 11 display selftests to pass, got $($r.Pass)" }
}

# the clipboard (last: closing it may leave the inventory open): right-click in the air opens it
Invoke-Chat '/give @s levitate:clipboard 1'
Press-Key '1'
if (-not $vanilla) {
    # U44: Create's "Clipboard Actions" tip and white outline on a block with settings
    $mark = Get-LogMark
    Invoke-Chat '/setblock ~ ~ ~2 levitate:creative_motor'
    Invoke-Chat '/tp @s ~ ~ ~ 0 35'
    Wait-LogLine 'clipboard: showing the Clipboard Actions tip \(2 lines\)' 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 600
    Save-Screenshot 'clipboard-tip' | Out-Null
}
Invoke-Chat '/tp @s ~ ~ ~ 0 -90'
Start-Sleep -Milliseconds 600
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
if ($vanilla) {
    Wait-LogLine 'screen display\.clipboard opened for .* \(form\)' 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 1500
    Save-Screenshot 'clipboard-form' | Out-Null
} else {
    Wait-LogLine 'screen display\.clipboard opened for .* \(native\)' 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 1200
    Save-Screenshot 'clipboard-screen' | Out-Null
}
if ($vanilla) { Close-Screen } else { Press-Key E; Start-Sleep -Milliseconds 800 }
