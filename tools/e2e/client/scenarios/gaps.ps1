# U37 gaps on a real client:
#   1. native text fields (gaps_text: frogport address, stock keeper search / address, clipboard);
#   2. kinetic feedback (gaps_feedback: goggles on, a generator's rotation indicators);
#   3. train controls from the keyboard (client mod only): `/create trains demo_manual` builds a
#      straight line with a one-carriage train (controls + steam whistle); an empty-hand right-click
#      at the controls starts controlling (trains.interact), W drives it out of the station, the
#      mouse wheel lowers the throttle, Ctrl honks, S brakes, Esc stops.
#   remote-vanilla: 1 and 2 only (forms; the vanilla train path is covered by trains.* selftests).
# Screenshots: out/gaps/{port-address,...,indicator-*,train-controlling,train-driving}.png
. (Join-Path $PSScriptRoot 'gaps_text.ps1')
. (Join-Path $PSScriptRoot 'gaps_feedback.ps1')

if ($script:Mode -ne 'remote-vanilla') {
    $mark = Get-LogMark
    Invoke-Chat '/gamemode creative'
    Invoke-Chat '/clear @s'
    Invoke-Chat '/time set noon'
    Invoke-Chat '/tp @s 0.5 -60 20.5'
    Start-Sleep -Milliseconds 600
    Invoke-Chat '/fill -2 -60 17 64 -54 23 air'
    Start-Sleep -Milliseconds 400
    Invoke-Chat '/create trains demo_manual'
    Wait-LogLine "trains: assembled train #\d+ '.*' with 1 carriages" 20 -Since $mark | Out-Null
    # stand west of the train and look at its controls (bogey at x 7, controls one block above it)
    Invoke-Chat '/tp @s 4.5 -60 20.5 facing 7.5 -57.5 20.5'
    Press-Key '1'
    Start-Sleep -Milliseconds 1000
    Click 0.5 0.5 -Relative -Right
    Wait-LogLine 'trains: .* is controlling train' 20 -Since $mark | Out-Null
    Wait-LogLine "trains client: controlling '.*'" 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 600
    Save-Screenshot 'train-controlling' | Out-Null
    # W: the train leaves the station (the player stays: the keys go to the train)
    Press-Key W 1200
    Wait-LogLine "trains: train #\d+ '.*' departing from 'Demo Manual'" 20 -Since $mark | Out-Null
    Save-Screenshot 'train-driving' | Out-Null
    # mouse wheel down five notches: 1 - 5/18 = 72 %
    Scroll -5
    Start-Sleep -Milliseconds 600
    Wait-LogLine "trains: train #\d+ '.*' throttle 72%" 20 -Since $mark | Out-Null
    # the sprint key sounds the whistle on the carriage
    Press-Key CTRL 800
    Wait-LogLine "trains: train #\d+ '.*' honks" 20 -Since $mark | Out-Null
    Press-Key S 1500
    # Esc stops controlling (and opens the pause menu, closed again)
    Press-Key ESC
    Wait-LogLine 'trains: .* stopped controlling train' 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 600
    Close-Screen
    Invoke-Chat '/fill -2 -60 17 64 -54 23 air'
}
Assert-NoErrors
