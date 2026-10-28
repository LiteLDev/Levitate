# U3 common on a real client:
#   local / remote-mod: the companion handshake, then the native list filter screen: open it with a
#                       right-click, pick stone from the hotbar and put it into the first slot.
#   remote-vanilla:     no handshake; the same right-click opens the form fallback.
# Screenshots: out/common/{filter-open,filter-set}.png or filter-form.png
$vanilla = $script:Mode -eq 'remote-vanilla'
if (-not $vanilla) {
    Wait-LogLine 'companion: server accepted the handshake' 90 | Out-Null
    Wait-LogLine 'companion joined: ' 30 | Out-Null
}

Invoke-Chat '/clear @s'
Invoke-Chat '/give @s stone 4'
Invoke-Chat '/give @s levitate:filter 1'
Invoke-Chat '/tp @s ~ ~ ~ 0 -90'   # look at the sky: a right-click uses the item
Press-Key '2'                      # hotbar slot 2: the filter (stone went to slot 1)
Start-Sleep -Milliseconds 600
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right

if ($vanilla) {
    Wait-LogLine 'screen common\.filter opened for .* \(form\)' 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 1500
    Save-Screenshot 'filter-form' | Out-Null
    Close-Screen
    return
}

$line = Wait-LogLine 'screen opened: .* panel=' 20 -Since $mark
Start-Sleep -Milliseconds 800
Save-Screenshot 'filter-open' | Out-Null
if ($line -notmatch 'panel=([\d.]+),([\d.]+) inv=([\d.]+),([\d.]+) gui=([\d.]+)x([\d.]+)') { throw "no layout in '$line'" }
$px = [double]$Matches[1]; $py = [double]$Matches[2]; $ix = [double]$Matches[3]; $iy = [double]$Matches[4]
$gw = [double]$Matches[5]; $gh = [double]$Matches[6]
# hotbar slot 0 (the stone) is the bottom row of the inventory panel; filter slot 0 at (7, 18)
$stoneX = ($ix + 8 + 9) / $gw;  $stoneY = ($iy + 8 + 3 * 18 + 4 + 9) / $gh
$slotX  = ($px + 7 + 9) / $gw;  $slotY  = ($py + 18 + 9) / $gh
Move-Mouse $stoneX $stoneY -Relative
Start-Sleep -Milliseconds 300
Click $stoneX $stoneY -Relative
Start-Sleep -Milliseconds 400
Move-Mouse $slotX $slotY -Relative
Start-Sleep -Milliseconds 300
Click $slotX $slotY -Relative
Wait-LogLine 'filter: .* set slot 0 to minecraft:stone' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 800
Save-Screenshot 'filter-set' | Out-Null
Close-Screen

if ($script:Mode -eq 'local') {
    $r = Invoke-SelfTest 'common.*'
    if ($r.Pass -lt 14) { throw "expected at least 14 common selftests to pass, got $($r.Pass)" }
}
