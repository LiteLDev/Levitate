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
# Shift-click the stone: straight into the next free ghost slot (1)
$crashStart = Get-Date
Move-Mouse $stoneX $stoneY -Relative
Start-Sleep -Milliseconds 300
[BsE2E.Native]::Key(0x10, $false)   # Shift down
Start-Sleep -Milliseconds 200
Click $stoneX $stoneY -Relative
Start-Sleep -Milliseconds 200
[BsE2E.Native]::Key(0x10, $true)    # Shift up
Wait-LogLine 'filter: .* set slot 1 to minecraft:stone' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 600
Save-Screenshot 'filter-shift' | Out-Null
Press-Key E                          # the inventory key closes our native screens
Start-Sleep -Milliseconds 1000

# The attribute filter: shift-click the stone as reference, then left-click it onto the cursor and
# into the reference slot, then add the first attribute it offers.
Invoke-Chat '/give @s levitate:attribute_filter 1'
Press-Key '3'                      # hotbar slot 3: the attribute filter
Start-Sleep -Milliseconds 600
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
$line = Wait-LogLine 'screen opened: .* panel=' 20 -Since $mark
if ($line -notmatch 'panel=([\d.]+),([\d.]+) inv=([\d.]+),([\d.]+) gui=([\d.]+)x([\d.]+)') { throw "no layout in '$line'" }
$px = [double]$Matches[1]; $py = [double]$Matches[2]; $ix = [double]$Matches[3]; $iy = [double]$Matches[4]
$gw = [double]$Matches[5]; $gh = [double]$Matches[6]
$stoneX = ($ix + 8 + 9) / $gw;  $stoneY = ($iy + 8 + 3 * 18 + 4 + 9) / $gh
$refX   = ($px + 8 + 9) / $gw;  $refY   = ($py + 20 + 9) / $gh
$addX   = ($px + 170 + 9) / $gw; $addY  = ($py + 20 + 9) / $gh
Move-Mouse $stoneX $stoneY -Relative
Start-Sleep -Milliseconds 300
[BsE2E.Native]::Key(0x10, $false)
Start-Sleep -Milliseconds 200
Click $stoneX $stoneY -Relative
Start-Sleep -Milliseconds 200
[BsE2E.Native]::Key(0x10, $true)
Wait-LogLine 'attribute filter: .* set the reference item to minecraft:stone' 20 -Since $mark | Out-Null
$mark2 = Get-LogMark
Click $stoneX $stoneY -Relative
Start-Sleep -Milliseconds 400
Move-Mouse $refX $refY -Relative
Start-Sleep -Milliseconds 300
Click $refX $refY -Relative
Wait-LogLine 'attribute filter: .* set the reference item to minecraft:stone' 20 -Since $mark2 | Out-Null
Start-Sleep -Milliseconds 800
Move-Mouse $addX $addY -Relative
Start-Sleep -Milliseconds 300
Click $addX $addY -Relative
Wait-LogLine 'attribute filter: .* added ' 20 -Since $mark2 | Out-Null
Start-Sleep -Milliseconds 800
Save-Screenshot 'attribute-filter' | Out-Null
$mark3 = Get-LogMark
Close-Screen                         # Esc closes native screens (and tells the server)
Wait-LogLine 'screen common\.attribute_filter closed by' 20 -Since $mark3 | Out-Null

# The text field: type into it, Enter submits; then Esc closes the screen.
$mark = Get-LogMark
Invoke-Chat '/create screentest'
$line = Wait-LogLine 'screen opened: .* panel=' 20 -Since $mark
if ($line -notmatch 'panel=([\d.]+),([\d.]+) inv=([\d.]+),([\d.]+) gui=([\d.]+)x([\d.]+)') { throw "no layout in '$line'" }
$px = [double]$Matches[1]; $py = [double]$Matches[2]; $gw = [double]$Matches[5]; $gh = [double]$Matches[6]
$fieldX = ($px + 8 + 80) / $gw; $fieldY = ($py + 20 + 8) / $gh
Move-Mouse $fieldX $fieldY -Relative
Start-Sleep -Milliseconds 300
Click $fieldX $fieldY -Relative
Start-Sleep -Milliseconds 400
Type-Text 'Levitate 123'
Start-Sleep -Milliseconds 400
Save-Screenshot 'text-field' | Out-Null
Press-Key ENTER
Wait-LogLine "screen test: .* submitted 'Levitate 123'" 20 -Since $mark | Out-Null
Close-Screen
Wait-LogLine 'screen common\.text_test closed by' 20 -Since $mark | Out-Null
# no crash while clicking items into the screens (the game asserts on ItemStack copies off its thread)
$crashDir = Join-Path $script:VersionDir 'logs\crash'
$crashes = @(Get-ChildItem $crashDir -File -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $crashStart })
if ($crashes) { throw "the game wrote $($crashes.Count) crash file(s) in $crashDir while using the filter screens" }
if (-not (Get-GameProcess)) { throw 'the game is gone after using the filter screens' }

if ($script:Mode -eq 'local') {
    $r = Invoke-SelfTest 'common.*'
    if ($r.Pass -lt 14) { throw "expected at least 14 common selftests to pass, got $($r.Pass)" }
}
