# Signals (U28) on a real client: a track with signal / observer blocks in every lamp state (the
# block states vanilla clients see), and a signal placed with the item the Create way (right-click
# the track, then right-click where the signal goes), bound to the track and showing green.
#   local / remote-mod: the client mod also draws the track marker (log "signals: rendering ...")
#   remote-vanilla:     the block lamps only
# Screenshots: out/signals/{lamps,selected,placed,above}.png
$mark = Get-LogMark
$withMod = $script:Mode -ne 'remote-vanilla'

# content errors about our blocks (signals.prelaunch.ps1 turned the content log file on)
$logDir = Join-Path (Get-GameDataDir) 'logs'
$contentLog = Get-ChildItem $logDir -Filter 'ContentLog*' -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $script:SignalsContentLogStart } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($contentLog) {
    $errors = Select-String -Path $contentLog.FullName -Pattern 'track_signal|track_observer' | ForEach-Object { $_.Line }
    if ($errors) {
        $errors | Set-Content (Join-Path (Join-Path $script:OutRoot $script:Unit) 'content-errors.txt')
        throw "content log reports $(@($errors).Count) problem(s) with the signal blocks (content-errors.txt): $($errors | Select-Object -First 3)"
    }
}

Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/fill ~-4 ~ ~-6 ~20 ~4 ~6 air'
Invoke-Chat '/fill ~2 ~ ~ ~18 ~ ~ levitate:track ["levitate:shape"=2]'
# lamps (fixed states, not bound): red, yellow, green entry signals, a green cross signal, a lit observer
Invoke-Chat '/setblock ~5 ~ ~-3 levitate:track_signal ["levitate:signal_state"=1]'
Invoke-Chat '/setblock ~7 ~ ~-3 levitate:track_signal ["levitate:signal_state"=2]'
Invoke-Chat '/setblock ~9 ~ ~-3 levitate:track_signal ["levitate:signal_state"=3]'
Invoke-Chat '/setblock ~11 ~ ~-3 levitate:track_signal ["levitate:signal_type"=1,"levitate:signal_state"=3]'
Invoke-Chat '/setblock ~13 ~ ~-3 levitate:track_observer ["levitate:powered"=1]'
Invoke-Chat '/give @s levitate:track_signal 4'
Press-Key 1
Invoke-Chat '/tp @s ~ ~ ~ -60 20'
Start-Sleep -Milliseconds 1200
Save-Screenshot 'lamps' | Out-Null

# right-click the track looking east (selects it, direction east)
Invoke-Chat '/tp @s ~ ~ ~ -90 25'
Start-Sleep -Milliseconds 800
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'signals: \S+ selected the track at' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 400
Save-Screenshot 'selected' | Out-Null

# right-click the ground two blocks south of the track: the signal is placed and bound
Invoke-Chat '/tp @s ~ ~ ~2 -90 25'
Start-Sleep -Milliseconds 800
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'signals: signal at \S+ bound to signal:' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1500
Invoke-Chat '/tp @s ~-2 ~2 ~4 -120 30'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'placed' | Out-Null
Invoke-Chat '/setblock ~4 ~5 ~-4 glass'
Invoke-Chat '/tp @s ~4 ~6 ~-4 0 90'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'above' | Out-Null

if ($withMod) { Wait-LogLine 'signals: rendering \d+ track marker' 20 -Since $mark | Out-Null }

$r = Invoke-SelfTest 'signals.*'
if ($r.Pass -lt 7) { throw "expected at least 7 signals selftests to pass, got $($r.Pass)" }
