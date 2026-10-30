# Native schedule editor (U38, Create's ScheduleScreen editor sub-screens) on a real client: add an
# action, open its instruction editor, type the station name into the native text field, confirm;
# then open the wait condition's editor, scroll the duration and confirm. The server stores both.
#   remote-vanilla: the same right-click opens the server form (no native editor).
# Screenshots: out/trains_schedule/{schedule-list,instruction-editor,condition-editor,schedule-edited,schedule-form}.png
$vanilla = $script:Mode -eq 'remote-vanilla'
$kind    = if ($vanilla) { 'form' } else { 'native' }
if (-not $vanilla) { Wait-LogLine 'companion joined: ' 90 | Out-Null }

# Clicks a point of the panel of the native screen opened since $mark (GUI pixels from the panel's
# top-left corner, as the screen's widget rectangles).
function Get-Panel([string]$mark) {
    $line = Wait-LogLine 'screen opened: .* panel=' 20 -Since $mark
    if ($line -notmatch 'panel=([\d.]+),([\d.]+) inv=([\d.]+),([\d.]+) gui=([\d.]+)x([\d.]+)') { throw "no layout in '$line'" }
    return @([double]$Matches[1], [double]$Matches[2], [double]$Matches[5], [double]$Matches[6])
}
function Point-Panel($panel, [double]$x, [double]$y) {
    $fx = ($panel[0] + $x) / $panel[2]
    $fy = ($panel[1] + $y) / $panel[3]
    Move-Mouse $fx $fy -Relative
    Start-Sleep -Milliseconds 300
    return @($fx, $fy)
}
function Click-Panel($panel, [double]$x, [double]$y) {
    $p = Point-Panel $panel $x $y
    Click $p[0] $p[1] -Relative
    Start-Sleep -Milliseconds 500
}

Invoke-Chat '/gamemode creative'
Invoke-Chat '/clear @s'
Invoke-Chat '/give @s levitate:schedule 1'
Press-Key 1
Invoke-Chat '/tp @s ~ ~ ~ 0 -60'
Start-Sleep -Milliseconds 800
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine "screen trains\.schedule opened for .* \($kind\)" 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1200
if ($vanilla) {
    Save-Screenshot 'schedule-form' | Out-Null
    Close-Screen
    return
}
$panel = Get-Panel $mark
# "+ Add Action" (8, 176, 70 x 18)
Click-Panel $panel 43 185
Start-Sleep -Milliseconds 800
Save-Screenshot 'schedule-list' | Out-Null
# the entry's edit button (180, 17, 14 x 12) opens the instruction editor
Click-Panel $panel 187 23
Wait-LogLine 'trains: schedule instruction editor opened for entry 1' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 500
# the station name field (8, 49, 240 x 14)
Click-Panel $panel 120 56
Type-Text 'Demo B'
Start-Sleep -Milliseconds 400
Save-Screenshot 'instruction-editor' | Out-Null
Press-Key ENTER
# confirm (226, 176, 22 x 18)
Click-Panel $panel 237 185
Wait-LogLine "trains: \S+ edited instruction 1 of a schedule: .*Demo B" 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 800
# the wait condition's edit button (208, 30, 14 x 12)
Click-Panel $panel 215 36
Wait-LogLine 'trains: schedule condition editor opened for entry 1' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 500
# the duration scroll input (8, 38, 240 x 14): one notch up, 5 -> 6 seconds
Point-Panel $panel 120 45 | Out-Null
Scroll 1
Start-Sleep -Milliseconds 400
Save-Screenshot 'condition-editor' | Out-Null
Click-Panel $panel 237 185
Wait-LogLine "trains: \S+ edited condition 1\.1\.1 of a schedule: .*6" 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 800
Save-Screenshot 'schedule-edited' | Out-Null
Press-Key E
Start-Sleep -Milliseconds 800
