# Schematics (U29) on a real client.
#   local / remote-mod: select an area with the schematic and quill (outline), name it in the native
#                       prompt, upload it at the schematic table (native screen), hold the written
#                       schematic (ghost preview following the cursor), deploy it, print it with a
#                       schematicannon (native screen, launched blocks), place a crafting blueprint
#                       and open its recipe screen.
#   remote-vanilla:     the same blocks without the client mod: forms for the quill name, the table
#                       and the cannon, particle outlines; no errors.
# Screenshots: out/schematics/*.png
$vanilla = $script:Mode -eq 'remote-vanilla'
if (-not $vanilla) {
    Wait-LogLine 'companion: server accepted the handshake' 90 | Out-Null
}

function Layout([string]$line) {
    if ($line -notmatch 'panel=([\d.]+),([\d.]+) inv=([\d.]+),([\d.]+) gui=([\d.]+)x([\d.]+)') { throw "no layout in '$line'" }
    return @{ px = [double]$Matches[1]; py = [double]$Matches[2]; ix = [double]$Matches[3]; iy = [double]$Matches[4]; gw = [double]$Matches[5]; gh = [double]$Matches[6] }
}
function Click-Gui($l, [double]$x, [double]$y) {
    $rx = $x / $l.gw; $ry = $y / $l.gh
    Move-Mouse $rx $ry -Relative; Start-Sleep -Milliseconds 300
    Click $rx $ry -Relative; Start-Sleep -Milliseconds 500
}
function Hotbar-Gui($l, [int]$slot) { return @(($l.ix + 8 + 18 * $slot + 9), ($l.iy + 8 + 3 * 18 + 4 + 9)) }
# our native screens close with the inventory key (Esc closes the host scene, which headless input may not reach)
function Close-Native { Press-Key E; Start-Sleep -Milliseconds 800 }

Invoke-Chat '/clear @s'
Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/tp @s 0.5 -60 0.5 0 30'
Start-Sleep -Seconds 1
# a small house to copy: 3x3 floor, two walls, a glass roof corner
Invoke-Chat '/fill 2 -61 4 4 -61 6 stone'
Invoke-Chat '/fill 2 -60 6 4 -59 6 oak_planks'
Invoke-Chat '/setblock 4 -60 4 oak_planks'
Invoke-Chat '/setblock 2 -59 4 glass'

# ---- schematic and quill ----
Invoke-Chat '/give @s levitate:schematic_and_quill'
Press-Key '1'
Invoke-Chat '/tp @s 0.5 -60 0.5 -36.87 17.95'
Start-Sleep -Milliseconds 600
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Start-Sleep -Milliseconds 800
Invoke-Chat '/tp @s 0.5 -60 0.5 -47.7 10.7'
Start-Sleep -Milliseconds 600
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'schematic and quill: .* selected \d+x\d+x\d+' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 800
Save-Screenshot 'quill-selection' | Out-Null
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
if ($vanilla) {
    Wait-LogLine 'screen schematics\.quill_name opened for .* \(form\)' 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 1500
    Save-Screenshot 'quill-name-form' | Out-Null
    Close-Screen
} else {
    Wait-LogLine 'screen opened: .* panel=' 20 -Since $mark | Out-Null
    Type-Text 'e2ehouse'               # the name field is a native text field (typed characters)
    Start-Sleep -Milliseconds 500
    Save-Screenshot 'quill-name' | Out-Null
    Press-Key ENTER
    Wait-LogLine 'schematic and quill: .* saved e2ehouse(_\d+)?\.mcstructure' 20 -Since $mark | Out-Null
    Wait-LogLine 'schematics client: saved e2ehouse(_\d+)?\.mcstructure to the local schematics folder' 20 -Since $mark | Out-Null
    Start-Sleep -Seconds 2
}

# ---- schematic table ----
Invoke-Chat '/setblock -2 -60 0 levitate:schematic_table ["levitate:facing"=3]'
Start-Sleep -Milliseconds 500
Invoke-Chat '/setblock -2 -60 0 levitate:schematic_table ["levitate:facing"=3]'
Invoke-Chat '/give @s levitate:empty_schematic'
Invoke-Chat '/tp @s 0.5 -60 0.5 90 29'
Start-Sleep -Milliseconds 800
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
if ($vanilla) {
    Wait-LogLine 'screen schematics\.table opened for .* \(form\)' 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 1500
    Save-Screenshot 'table-form' | Out-Null
    Close-Screen
} else {
    $l = Layout (Wait-LogLine 'screen opened: .* panel=' 20 -Since $mark)
    Start-Sleep -Milliseconds 800
    Save-Screenshot 'table-open' | Out-Null
    $hx, $hy = Hotbar-Gui $l 1
    Click-Gui $l $hx $hy
    Wait-LogLine 'schematic table at .* put an empty schematic in' 20 -Since $mark | Out-Null
    # pick our file: the selection input cycles with the wheel; the first file is fine when it is ours
    Click-Gui $l ($l.px + 172 + 9) ($l.py + 30 + 9)
    Wait-LogLine 'schematics: new schematic uploaded: .*/[^ ]+\.mcstructure' 40 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 1000
    Save-Screenshot 'table-uploaded' | Out-Null
    Click-Gui $l ($l.px + 188 + 9) ($l.py + 70 + 9)
    Close-Native
}

if ($vanilla) {
    # vanilla: the cannon form and the particle outlines are enough
    Invoke-Chat '/setblock 0 -60 -3 levitate:schematicannon'
    Invoke-Chat '/tp @s 0.5 -60 0.5 180 20'
    Start-Sleep -Milliseconds 800
    $mark = Get-LogMark
    Click 0.5 0.5 -Relative -Right
    Wait-LogLine 'screen schematics\.cannon opened for .* \(form\)' 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 1500
    Save-Screenshot 'cannon-form' | Out-Null
    Close-Screen
    Assert-NoErrors
    return
}

# ---- written schematic: ghost preview and deploying ----
Press-Key '2'
Invoke-Chat '/tp @s 0.5 -60 0.5 180 16'
$mark = Get-LogMark
Start-Sleep -Seconds 2
Wait-LogLine 'schematics client: received the preview of ' 20 | Out-Null
Wait-LogLine 'schematics client: drawing the ghost of ' 20 | Out-Null
Save-Screenshot 'ghost-follow' | Out-Null
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'schematic: .* placed .* at -?\d+ -?\d+ -?\d+' 20 -Since $mark | Out-Null
Start-Sleep -Seconds 1
Save-Screenshot 'ghost-deployed' | Out-Null

# ---- schematicannon ----
Invoke-Chat '/setblock 3 -60 -3 levitate:schematicannon'
Invoke-Chat '/setblock 4 -60 -3 chest'
Invoke-Chat '/replaceitem block 4 -60 -3 slot.container 0 stone 64'
Invoke-Chat '/replaceitem block 4 -60 -3 slot.container 1 oak_planks 64'
Invoke-Chat '/replaceitem block 4 -60 -3 slot.container 2 glass 64'
Invoke-Chat '/replaceitem block 4 -60 -3 slot.container 3 gunpowder 8'
Invoke-Chat '/tp @s 0.5 -60 0.5 -135 14.8'
Start-Sleep -Milliseconds 800
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
$l = Layout (Wait-LogLine 'screen opened: .* panel=' 20 -Since $mark)
$hx, $hy = Hotbar-Gui $l 1
Click-Gui $l $hx $hy
Start-Sleep -Seconds 2
Save-Screenshot 'cannon-ready' | Out-Null
Click-Gui $l ($l.px + 75 + 9) ($l.py + 50 + 9)
Wait-LogLine 'schematicannon at .*: started' 20 -Since $mark | Out-Null
Close-Native
Invoke-Chat '/tp @s 0.5 -58 4.5 180 15.6'
Start-Sleep -Seconds 2
Save-Screenshot 'cannon-printing' | Out-Null
Wait-LogLine 'schematicannon at .*: finished printing ' 240 -Since $mark | Out-Null
Start-Sleep -Seconds 2
Save-Screenshot 'cannon-finished' | Out-Null

# ---- crafting blueprint ----
Invoke-Chat '/fill -6 -60 3 -6 -58 5 stone'
Invoke-Chat '/give @s levitate:crafting_blueprint'
Press-Key '2'   # the written schematic went into the cannon: the blueprint takes its slot
Invoke-Chat '/tp @s -3.5 -59 4.5 90 0'
Start-Sleep -Milliseconds 800
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'crafting blueprint placed at .* \(\d+x\d+\)' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 800
Save-Screenshot 'blueprint-placed' | Out-Null
Press-Key '3'   # an empty hand opens the recipe screen
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'screen opened: .* panel=' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 800
Save-Screenshot 'blueprint-screen' | Out-Null
Close-Native

if ($script:Mode -eq 'local') {
    $r = Invoke-SelfTest 'schematics.*'
    if ($r.Pass -lt 10) { throw "expected at least 10 schematics selftests to pass, got $($r.Pass)" }
}
Assert-NoErrors
