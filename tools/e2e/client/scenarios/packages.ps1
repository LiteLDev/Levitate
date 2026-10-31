# U25 packages on a real client:
#   local / remote-mod: connect two chain conveyors with chains (right-clicks), hang a package on the
#                       turning conveyor, screenshot the chains and the box (client mod rendering),
#                       then open a frogport's native port screen.
#   remote-vanilla:     the frogport's right-click opens the form fallback.
# Screenshots: out/packages/{chains,port-screen}.png or port-form.png
$vanilla = $script:Mode -eq 'remote-vanilla'
if (-not $vanilla) {
    Wait-LogLine 'companion: server accepted the handshake' 90 | Out-Null
}

Invoke-Chat '/gamemode creative'
Invoke-Chat '/clear @s'
Invoke-Chat '/tp @s 0 -60 0'
Start-Sleep -Milliseconds 800

# --- U46: package entities (every mode: the entity's model comes with the resource pack) ---------
Invoke-Chat '/gamemode survival'
Invoke-Chat '/kill @e[type=levitate:package]'
Invoke-Chat '/replaceitem entity @s slot.hotbar 0 levitate:cardboard_package_12x12 1'
Invoke-Chat '/replaceitem entity @s slot.hotbar 1 levitate:rare_creeper_package 1'
Invoke-Chat '/tp @s 0.5 -60 0.5 0 50'
Press-Key '1'
Start-Sleep -Milliseconds 1200
# right-click on the ground: the package is put down as an entity
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'packages: .* put down a package at' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1500
Save-Screenshot 'package-put-down' | Out-Null
# an empty hand takes it back
Press-Key '9'
Start-Sleep -Milliseconds 600
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'packages: .* picked up a package entity' 20 -Since $mark | Out-Null
# put it down again and break it open with a hit
Press-Key '9'
Invoke-Chat '/replaceitem entity @s slot.hotbar 0 levitate:cardboard_package_12x12 1'
Press-Key '1'
Start-Sleep -Milliseconds 800
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'packages: .* put down a package at' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 800
Click 0.5 0.5 -Relative
Wait-LogLine 'packages: a package entity broke open' 20 -Since $mark | Out-Null
# hold right-click with the rare package and let go: it is thrown
Press-Key '2'
Invoke-Chat '/tp @s 0.5 -60 0.5 0 -10'
Start-Sleep -Milliseconds 1000
$mark = Get-LogMark
[BsE2E.Native]::MouseButton(1, $false)
Start-Sleep -Milliseconds 450   # a short charge: a gentle throw a few blocks ahead
[BsE2E.Native]::MouseButton(1, $true)
Wait-LogLine 'packages: .* threw a package' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 150
Save-Screenshot 'package-thrown' | Out-Null
Start-Sleep -Milliseconds 2000
Invoke-Chat '/tp @s 0.5 -58 -2 0 30'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'package-landed' | Out-Null
Invoke-Chat '/kill @e[type=levitate:package]'
Invoke-Chat '/gamemode creative'
Invoke-Chat '/clear @s'
Invoke-Chat '/tp @s 0 -60 0'
Start-Sleep -Milliseconds 800

# --- U56: a frogport placed from the side faces the player (levitate:yaw, 22.5 degree steps) ------
Invoke-Chat '/replaceitem entity @s slot.hotbar 4 levitate:package_frogport 1'
Press-Key '5'
Invoke-Chat '/tp @s 7.5 -60 -0.5 facing 5.5 -61 -2.5'
Start-Sleep -Milliseconds 1200
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
$line = Wait-LogLine 'packages: frogport placed at 0:-?\d+:-?\d+:-?\d+' 20 -Since $mark
$m = [regex]::Match($line, 'placed at 0:(-?\d+):(-?\d+):(-?\d+)')
$fx = [int]$m.Groups[1].Value; $fy = [int]$m.Groups[2].Value; $fz = [int]$m.Groups[3].Value
Invoke-Chat "/tp @s $($fx + 0.5) $($fy + 3.5) $($fz + 0.5) facing $($fx + 0.5) $fy $($fz + 0.5)"
Start-Sleep -Milliseconds 1500
Save-Screenshot 'frogport-yaw' | Out-Null   # seen from above: the frog's eyes point towards +x +z (the player)
Invoke-Chat "/setblock $fx $fy $fz air"
Invoke-Chat '/replaceitem entity @s slot.hotbar 4 air'   # (creative keeps the placed item)
Invoke-Chat '/tp @s 0 -60 0'
Start-Sleep -Milliseconds 600

# --- the port screen ---------------------------------------------------------------------------
Invoke-Chat '/setblock 5 -60 -3 levitate:package_frogport'
Invoke-Chat '/tp @s 5.5 -60 0.5 180 25'
Press-Key '9'                      # an empty hotbar slot
Start-Sleep -Milliseconds 800
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
if ($vanilla) {
    Wait-LogLine 'screen packages\.port opened for .* \(form\)' 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 1500
    Save-Screenshot 'port-form' | Out-Null
    Close-Screen
    Invoke-Chat '/setblock 5 -60 -3 air'
    return
}
Wait-LogLine 'screen packages\.port opened for .* \(native\)' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1200
Save-Screenshot 'port-screen' | Out-Null
Press-Key E                        # the inventory key closes native screens (Esc goes to the host scene)
Start-Sleep -Milliseconds 800
Invoke-Chat '/setblock 5 -60 -3 air'

# --- chain conveyors -------------------------------------------------------------------------------
Invoke-Chat '/setblock 3 -60 6 levitate:chain_conveyor'
Invoke-Chat '/setblock 11 -60 6 levitate:chain_conveyor'
Invoke-Chat '/setblock 3 -61 6 levitate:kinetics_test_source ["levitate:axis"=1]'
Invoke-Chat '/give @s iron_chain 16'
Invoke-Chat '/give @s levitate:cardboard_package_12x12 1'
# a package first (a plain item), then the chains (a block item)
Press-Key '2'
Start-Sleep -Milliseconds 600
$mark = Get-LogMark
Invoke-Chat '/tp @s 3.5 -60 2.5 0 17'
Start-Sleep -Milliseconds 1200
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'packages: .* hung a package for .* on the chain conveyor at 0:3:-60:6' 20 -Since $mark | Out-Null
# placing a block item by hand (the frogport, with no target selected)
Invoke-Chat '/give @s levitate:package_frogport 1'
Press-Key '3'
Invoke-Chat '/tp @s 0.5 -60 0.5 180 45'
Start-Sleep -Milliseconds 1200
Click 0.5 0.5 -Relative -Right
Start-Sleep -Milliseconds 1500
Save-Screenshot 'frogport-placed' | Out-Null
Wait-LogLine 'packages: frogport placed at' 10 -Since $mark | Out-Null
Press-Key '1'
Start-Sleep -Milliseconds 600
Invoke-Chat '/tp @s 3.5 -60 2.5 0 17'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'aim-first' | Out-Null
Click 0.5 0.5 -Relative -Right
Start-Sleep -Milliseconds 1000
Save-Screenshot 'after-first' | Out-Null
Wait-LogLine 'packages: .* selected the chain conveyor at 0:3:-60:6' 20 -Since $mark | Out-Null
Invoke-Chat '/tp @s 11.5 -60 2.5 0 17'
Start-Sleep -Milliseconds 1500
$connected = $false
for ($try = 0; $try -lt 3 -and -not $connected; $try++) {
    Click 0.5 0.5 -Relative -Right
    try { Wait-LogLine 'packages: chain conveyors at 0:3:-60:6 and 0:11:-60:6 connected' 6 -Since $mark | Out-Null; $connected = $true } catch { Save-Screenshot "aim-second-$try" | Out-Null }
}
if (-not $connected) { throw 'the chain conveyors did not connect' }


Invoke-Chat '/tp @s 7.5 -57 0.5 0 34'
Wait-LogLine 'packages: rendering chain conveyors' 30 -Since $mark | Out-Null
Start-Sleep -Milliseconds 2500
Save-Screenshot 'chains' | Out-Null

Invoke-Chat '/setblock 3 -61 6 grass_block'
Invoke-Chat '/setblock 3 -60 6 air'
Invoke-Chat '/setblock 11 -60 6 air'
Invoke-Chat '/kill @e[type=item]'

if ($script:Mode -eq 'local') {
    $r = Invoke-SelfTest 'packages.*'
    if ($r.Pass -lt 11) { throw "expected at least 11 packages selftests to pass, got $($r.Pass)" }
}
