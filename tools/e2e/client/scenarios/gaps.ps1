# U37 gaps on a real client:
#   1. native text fields (gaps_text: frogport address, stock keeper search / address, clipboard);
#   2. kinetic feedback (gaps_feedback: goggles on, a generator's rotation indicators);
#   3. train controls from the keyboard (client mod only): on a straight line, `/create trains
#      demo_manual` builds a station with a one-carriage train (controls + steam whistle); an empty-hand right-click
#      at the controls starts controlling (trains.interact), W drives it out of the station, the
#      mouse wheel lowers the throttle, Ctrl honks, S brakes, Esc stops.
#   Before 3, every mode checks side-on that the demo carriage sits on its bogey (gaps_seat).
#   remote-vanilla: 1, 2 and the carriage check (forms; the vanilla train path is covered by
#   trains.* selftests).
# Screenshots: out/gaps/{port-address,...,indicator-*,train-side,train-controlling,train-driving}.png
. (Join-Path $PSScriptRoot 'gaps_text.ps1')
. (Join-Path $PSScriptRoot 'gaps_feedback.ps1')
. (Join-Path $PSScriptRoot 'gaps_seat.ps1')

# the demo train, seen side-on: its carriage sits on the bogey (every mode)
$mark = Assert-TrainSeated

if ($script:Mode -ne 'remote-vanilla') {
    $at = Wait-LogLine 'trains: demo train #\d+ has its controls at' 20 -Since $mark
    if ($at -notmatch 'controls at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)') { throw "no controls position in '$at'" }
    $inv = [cultureinfo]::InvariantCulture
    $cx = [double]::Parse($Matches[1], $inv); $cy = [double]::Parse($Matches[2], $inv); $cz = [double]::Parse($Matches[3], $inv)
    # stand on the ground 3 blocks west and 1 south of the controls (beside the track: a track block
    # pushes the player out) and look at them (Bedrock yaw: -atan2(dx, dz))
    $px = $cx - 3; $pz = $cz + 1
    $yaw   = -[math]::Atan2($cx - $px, $cz - $pz) * 180 / [math]::PI
    $pitch = -[math]::Atan2($cy - (-60 + 1.62), [math]::Sqrt(10)) * 180 / [math]::PI
    $aim = '/tp @s {0} -60 {1} {2} {3}' -f $px.ToString('0.00', $inv), $pz.ToString('0.00', $inv), $yaw.ToString('0.0', $inv), $pitch.ToString('0.0', $inv)
    Press-Key '1'
    # the cursor goes to the centre first (moving it turns the camera), then the aim is set again
    Move-Mouse 0.5 0.5 -Relative
    Start-Sleep -Milliseconds 400
    Invoke-Chat $aim
    Start-Sleep -Milliseconds 1000
    Save-Screenshot 'train-aim' | Out-Null
    Click 0.5 0.5 -Relative -Right
    Wait-LogLine 'trains: .* is controlling train' 20 -Since $mark | Out-Null
    Wait-LogLine "trains client: controlling '.*'" 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 600
    Save-Screenshot 'train-controlling' | Out-Null
    # mouse wheel down five notches, one at a time: 1 - 5/18 = 72 %
    foreach ($i in 1..5) { Scroll -1; Start-Sleep -Milliseconds 300 }
    Wait-LogLine "trains: train #\d+ '.*' throttle 72%" 20 -Since $mark | Out-Null
    # the sprint key sounds the whistle on the carriage
    Press-Key CTRL 800
    Wait-LogLine "trains: train #\d+ '.*' honks" 20 -Since $mark | Out-Null
    # W: the train leaves the station (the player stays: the keys go to the train); S brakes before
    # it gets out of the driver's 8 blocks
    Press-Key W 700
    Wait-LogLine "trains: train #\d+ '.*' departing from 'Demo Manual'" 20 -Since $mark | Out-Null
    Save-Screenshot 'train-driving' | Out-Null
    Press-Key S 1500
    # Esc stops controlling (and opens the pause menu, closed again)
    Press-Key ESC
    Wait-LogLine 'trains: .* stopped controlling train' 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 600
    Close-Screen
    Invoke-Chat '/fill -2 -60 17 64 -54 23 air'
}
Assert-NoErrors
