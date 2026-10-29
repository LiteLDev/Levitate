# U37 native text fields on a real client: type into the native screens' text boxes (headless
# input sends WM_CHAR text, as a keyboard / IME would):
#   - the frogport's address box (U25), Enter sends it -> the server's port catches 'Mill';
#   - the stock keeper's search and address boxes (U26);
#   - the clipboard's first entry (U24).
#   remote-vanilla: the same right-clicks open the forms (nothing to type natively).
# Screenshots: out/gaps_text/{port-address,keeper-search,keeper-address,clipboard-typed}.png
$vanilla = $script:Mode -eq 'remote-vanilla'
$kind    = if ($vanilla) { 'form' } else { 'native' }
if (-not $vanilla) { Wait-LogLine 'companion joined: ' 90 | Out-Null }
$crashStart = Get-Date

# Clicks a point of the panel of the native screen opened since $mark (GUI pixels from the panel's
# top-left corner, as the screen's widget rectangles).
function Click-Panel([string]$mark, [double]$x, [double]$y) {
    $line = Wait-LogLine 'screen opened: .* panel=' 20 -Since $mark
    if ($line -notmatch 'panel=([\d.]+),([\d.]+) inv=([\d.]+),([\d.]+) gui=([\d.]+)x([\d.]+)') { throw "no layout in '$line'" }
    $fx = ([double]$Matches[1] + $x) / [double]$Matches[5]
    $fy = ([double]$Matches[2] + $y) / [double]$Matches[6]
    Move-Mouse $fx $fy -Relative
    Start-Sleep -Milliseconds 300
    Click $fx $fy -Relative
    Start-Sleep -Milliseconds 400
}
function Close-Native { if ($vanilla) { Close-Screen } else { Press-Key E; Start-Sleep -Milliseconds 800 } }

Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/clear @s'
Invoke-Chat '/tp @s 0 -60 0'
Start-Sleep -Milliseconds 800

# --- frogport address (U25) ----------------------------------------------------------------------
Invoke-Chat '/setblock 5 -60 -3 levitate:package_frogport'
Invoke-Chat '/tp @s 5.5 -60 0.5 180 25'
Press-Key '9'
Start-Sleep -Milliseconds 800
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine "screen packages\.port opened for .* \($kind\)" 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1000
if ($vanilla) {
    Save-Screenshot 'port-form' | Out-Null
    Close-Screen
} else {
    Click-Panel $mark (7 + 81) (28 + 7)
    Type-Text 'Mill'
    Start-Sleep -Milliseconds 400
    Save-Screenshot 'port-address' | Out-Null
    Press-Key ENTER
    Wait-LogLine "package port address typed: 'Mill'" 20 -Since $mark | Out-Null
    Wait-LogLine "packages: port at .* now catches 'Mill'" 20 -Since $mark | Out-Null
    Close-Native
}
Invoke-Chat '/setblock 5 -60 -3 air'

# --- stock keeper search and address (U26) ---------------------------------------------------------
Invoke-Chat '/tp @s 0 -60 0 facing 3 -59 0'
Start-Sleep -Milliseconds 500
Invoke-Chat '/setblock ~3 ~ ~ levitate:stock_ticker ["levitate:facing"=1]'
Invoke-Chat '/kill @e[type=pig]'                 # only our keeper (no stray mob for c=1 below)
Invoke-Chat '/kill @e[type=boat]'
Invoke-Chat '/summon boat ~3 ~ ~1'
Invoke-Chat '/summon pig ~3 ~ ~1'
Start-Sleep -Milliseconds 500
Invoke-Chat '/ride @e[type=pig,c=1] start_riding @e[type=boat,c=1] teleport_rider'
Start-Sleep -Milliseconds 800
Invoke-Chat '/tp @s ~ ~ ~ facing ~3 ~-0.9 ~1'
Start-Sleep -Milliseconds 600
$mark   = Get-LogMark
$opened = $false
for ($try = 0; $try -lt 3 -and -not $opened; $try++) {
    Click 0.5 0.5 -Relative -Right
    try { Wait-LogLine "screen stock\.request opened for .* \($kind\)" 10 -Since $mark | Out-Null; $opened = $true } catch { Save-Screenshot "keeper-aim-$try" | Out-Null }
}
if (-not $opened) { throw 'the stock keeper screen did not open' }
Start-Sleep -Milliseconds 1000
if ($vanilla) {
    Save-Screenshot 'keeper-form' | Out-Null
    Close-Screen
} else {
    Click-Panel $mark (12 + 70) (15 + 7)          # the search box
    Type-Text 'log'
    Start-Sleep -Milliseconds 600
    Save-Screenshot 'keeper-search' | Out-Null
    Press-Key ENTER
    Wait-LogLine "stock keeper search typed: 'log'" 20 -Since $mark | Out-Null
    Click-Panel $mark (12 + 81) (150 + 8)         # the address box
    Type-Text 'Mill'
    Start-Sleep -Milliseconds 400
    Press-Key ENTER
    Wait-LogLine "stock keeper address typed: 'Mill'" 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 600
    Save-Screenshot 'keeper-address' | Out-Null
    Close-Native
}
Invoke-Chat '/kill @e[type=pig]'
Invoke-Chat '/kill @e[type=boat]'

# --- clipboard entry (U24; last: closing it may leave the inventory open) ---------------------------
Invoke-Chat '/clear @s'
Invoke-Chat '/give @s levitate:clipboard 1'
Invoke-Chat '/tp @s ~ ~ ~ 0 -90'
Press-Key '1'
Start-Sleep -Milliseconds 600
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine "screen display\.clipboard opened for .* \($kind\)" 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1000
if ($vanilla) {
    Save-Screenshot 'clipboard-form' | Out-Null
    Close-Screen
} else {
    Click-Panel $mark (22 + 85) (31 + 6)          # the first (blank) entry
    Type-Text 'Iron ingots'
    Start-Sleep -Milliseconds 400
    Press-Key ENTER
    Wait-LogLine 'clipboard page 1 typed' 20 -Since $mark | Out-Null
    Wait-LogLine 'clipboard page 1 edited by .* \(1 entries\)' 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 800
    Save-Screenshot 'clipboard-typed' | Out-Null
    Close-Native
}

$crashDir = Join-Path $script:VersionDir 'logs\crash'
$crashes = @(Get-ChildItem $crashDir -File -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $crashStart })
if ($crashes) { throw "the game wrote $($crashes.Count) crash file(s) in $crashDir while typing into the screens" }
if (-not (Get-GameProcess)) { throw 'the game is gone after typing into the screens' }

if ($script:Mode -eq 'local') {
    $r = Invoke-SelfTest 'gaps.textfield_*'
    if ($r.Pass -lt 2) { throw "expected the 2 gaps.textfield selftests to pass, got $($r.Pass)" }
}
