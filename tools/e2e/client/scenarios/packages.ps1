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

# --- the port screen ---------------------------------------------------------------------------
Invoke-Chat '/setblock 5 -60 -3 levitate:package_frogport'
Invoke-Chat '/tp @s 5.5 -60 0.5 facing 5.5 -59.6 -2.5'
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
Close-Screen
Invoke-Chat '/setblock 5 -60 -3 air'

# --- chain conveyors -------------------------------------------------------------------------------
Invoke-Chat '/setblock 3 -60 6 levitate:chain_conveyor'
Invoke-Chat '/setblock 11 -60 6 levitate:chain_conveyor'
Invoke-Chat '/setblock 3 -61 6 levitate:kinetics_test_source ["levitate:axis"=1]'
Invoke-Chat '/give @s iron_chain 16'
Invoke-Chat '/give @s levitate:cardboard_package_12x12 1'
Press-Key '1'
Start-Sleep -Milliseconds 600
$mark = Get-LogMark
Invoke-Chat '/tp @s 7.5 -60 1.5 facing 3.5 -59.5 6.5'
Start-Sleep -Milliseconds 600
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'packages: .* selected the chain conveyor at 0:3:-60:6' 20 -Since $mark | Out-Null
Invoke-Chat '/tp @s 7.5 -60 1.5 facing 11.5 -59.5 6.5'
Start-Sleep -Milliseconds 600
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'packages: chain conveyors at 0:3:-60:6 and 0:11:-60:6 connected' 20 -Since $mark | Out-Null

# hang a package on the first conveyor's loop
Press-Key '2'
Invoke-Chat '/tp @s 3.5 -60 2.5 facing 3.5 -59.6 6.5'
Start-Sleep -Milliseconds 600
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'packages: .* hung a package for .* on the chain conveyor at 0:3:-60:6' 20 -Since $mark | Out-Null

Invoke-Chat '/tp @s 7.5 -57 0.5 facing 7.5 -59.5 6.5'
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
