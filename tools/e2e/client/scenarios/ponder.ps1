# U30 Ponder on a real client:
#   1. hover a mechanical piston in the inventory and hold W: its Ponder opens (scene player);
#   2. the scene plays (blocks, text bubbles), D = next scene, Q = identify, S = replay, Esc closes;
#   3. `/create ponder` opens the index of categories, `/create ponder kinetic_relays` a tag screen;
#   4. `/create ponder` of an entry whose blocks are not in main yet still plays (placeholders);
#   5. the server-side selftests (`ponder.*`).
# Screenshots: out/ponder/*.png
Wait-LogLine 'companion: server accepted the handshake' 90 | Out-Null

Invoke-Chat '/gamemode survival'
Invoke-Chat '/clear @s'
foreach ($i in 0..8) { Invoke-Chat "/replaceitem entity @s slot.hotbar $i levitate:mechanical_piston 1" }
Invoke-Chat '/tp @s ~ ~ ~ 0 0'
Start-Sleep -Milliseconds 500

# ---- 1. hold W over the item -------------------------------------------------------------------
$mark = Get-LogMark
Open-Inventory
Save-Screenshot 'inventory' | Out-Null
$found = $false
# the survival inventory at 1600x900: hotbar row at y ~0.73, slots from x ~0.506 every ~0.0336
:scan foreach ($y in 0.73, 0.66, 0.60, 0.54, 0.80) {
    foreach ($i in 0..8) {
        Move-Mouse (0.506 + 0.0336 * $i) $y -Relative
        Start-Sleep -Milliseconds 200
        if (Get-LeviLog -Since $mark | Where-Object { $_ -match 'ponder: hovering levitate:mechanical_piston' }) { $found = $true; break scan }
    }
}
if (-not $found) { Save-Screenshot 'hover-failed' | Out-Null; throw 'no inventory slot with the piston was hovered' }
Start-Sleep -Milliseconds 300
Save-Screenshot 'hover-hint' | Out-Null
# U44: while W is held the tooltip line becomes Create's progress bar
[BsE2E.Native]::Key(0x57, $false)   # W down
Start-Sleep -Milliseconds 150
Save-Screenshot 'hold-progress' | Out-Null
[BsE2E.Native]::Key(0x57, $true)    # W up: the bar drains again
Start-Sleep -Milliseconds 1200
Press-Key W 1600
$holdFailed = $null
try { Wait-LogLine 'ponder: held W over levitate:mechanical_piston' 15 -Since $mark | Out-Null }
catch {
    # keep going (the scene player is checked below either way); the run fails at the end
    $holdFailed = $_.Exception.Message
    Save-Screenshot 'hold-failed' | Out-Null
    Close-Screen
    Invoke-Chat '/create ponder mechanical_piston'
}
Wait-LogLine 'ponder: scene mechanical_piston \(1/3\) of levitate:mechanical_piston' 15 -Since $mark | Out-Null
Wait-LogLine 'ponder: scene mechanical_piston drawn' 15 -Since $mark | Out-Null
Start-Sleep -Milliseconds 2500
Save-Screenshot 'piston-start' | Out-Null
Start-Sleep -Seconds 4
Save-Screenshot 'piston-text' | Out-Null

# ---- 2. controls -------------------------------------------------------------------------------
$mark = Get-LogMark
Press-Key D
Wait-LogLine 'ponder: scene piston_pole \(2/3\)' 10 -Since $mark | Out-Null
Start-Sleep -Seconds 4
Save-Screenshot 'piston-pole' | Out-Null
Press-Key Q
Wait-LogLine 'ponder: identify mode on' 10 -Since $mark | Out-Null
Move-Mouse 0.5 0.55 -Relative
Start-Sleep -Milliseconds 600
Save-Screenshot 'identify' | Out-Null
Press-Key S
Wait-LogLine 'ponder: replay piston_pole' 10 -Since $mark | Out-Null
# drag to turn the camera
Move-Mouse 0.45 0.5 -Relative
[BsE2E.Native]::MouseButton(0, $false); Start-Sleep -Milliseconds 100   # down
foreach ($i in 1..10) { Move-Mouse (0.45 + $i * 0.02) 0.5 -Relative; Start-Sleep -Milliseconds 50 }
[BsE2E.Native]::MouseButton(0, $true)   # up
Start-Sleep -Seconds 2
Save-Screenshot 'dragged' | Out-Null
Close-Screen
Wait-LogLine 'ponder: closed levitate:mechanical_piston' 10 -Since $mark | Out-Null
if (-not $holdFailed) { Close-Screen }   # the inventory below

# ---- 3. index and tags -------------------------------------------------------------------------
$mark = Get-LogMark
Invoke-Chat '/create ponder'
Wait-LogLine 'ponder: index of \d+ categories opened' 15 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1200
Save-Screenshot 'index' | Out-Null
Close-Screen
$mark = Get-LogMark
Invoke-Chat '/create ponder kinetic_relays'
Wait-LogLine 'ponder: tag kinetic_relays opened' 15 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1200
Save-Screenshot 'tag' | Out-Null
Close-Screen

# ---- 4. other entries (some blocks may not exist yet: placeholders) ----------------------------
foreach ($entry in 'shaft', 'hand_crank', 'track_station') {
    $mark = Get-LogMark
    Invoke-Chat "/create ponder $entry"
    Wait-LogLine "ponder: opened levitate:$entry" 15 -Since $mark | Out-Null
    Wait-LogLine 'ponder: scene \w+ drawn' 15 -Since $mark | Out-Null
    Start-Sleep -Seconds 5
    Save-Screenshot "entry-$entry" | Out-Null
    Close-Screen
}

# ---- 5. selftests ------------------------------------------------------------------------------
$r = Invoke-SelfTest 'ponder.*'
if ($r.Pass -lt 3) { throw "expected 3 ponder selftests to pass, got $($r.Pass)" }

if ($holdFailed) { throw "holding W over the item did not open Ponder: $holdFailed" }
