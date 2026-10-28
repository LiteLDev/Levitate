# Tracks (U5) on a real client: place two tracks, connect them with a curve by right-clicking
# with the track item (select the first end, click the second), and screenshot the curve.
#   local / remote-mod: the client mod draws the curve (log "tracks: rendering ...")
#   remote-vanilla:     only the straight tracks and the track bed (fake tracks) are visible
# Screenshots: out/tracks/{placed,selected,curve,curve-above,curve-close}.png
$mark = Get-LogMark
$withMod = $script:Mode -ne 'remote-vanilla'

# content errors about our packs (tracks.prelaunch.ps1 turned the content log file on)
$logDir = Join-Path (Get-GameDataDir) 'logs'
$contentLog = Get-ChildItem $logDir -Filter 'ContentLog*' -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $script:TracksContentLogStart } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($contentLog) {
    $errors = Select-String -Path $contentLog.FullName -Pattern 'levitate:(fake_|incomplete_)?track|levitate_(fake_|incomplete_)?track|track.geo|(fake_)?track.json' | ForEach-Object { $_.Line }
    if ($errors) {
        $errors | Set-Content (Join-Path (Join-Path $script:OutRoot $script:Unit) 'content-errors.txt')
        throw "content log reports $(@($errors).Count) problem(s) with the Levitate packs (content-errors.txt): $($errors | Select-Object -First 3)"
    }
}

Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
# a clean flat area east of the player, then the two ends: east-west at +3, north-south at (+12, -9)
Invoke-Chat '/fill ~-4 ~ ~-14 ~16 ~4 ~4 air'
Invoke-Chat '/setblock ~3 ~ ~ levitate:track ["levitate:shape"=2]'
Invoke-Chat '/setblock ~12 ~ ~-9 levitate:track ["levitate:shape"=1]'
Invoke-Chat '/give @s levitate:track 64'
Press-Key 1
# look east, 25 degrees down: the crosshair is on the first track
Invoke-Chat '/tp @s ~ ~ ~ -90 25'
Start-Sleep -Milliseconds 800
Save-Screenshot 'placed' | Out-Null

# right-click the first track: selects its east end
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'tracks: \S+ selected the track at' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 400
Save-Screenshot 'selected' | Out-Null

# stand 3 blocks south of the second track looking north and down at it, right-click: a curve
Invoke-Chat '/tp @s ~12 ~ ~-6 180 28'
Start-Sleep -Milliseconds 800
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'tracks: \S+ connected .* with a curve of length' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1500

# view the curve from the south-west and from above
Invoke-Chat '/tp @s ~-12 ~4 ~10 -135 30'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'curve' | Out-Null
Invoke-Chat '/setblock ~7 ~7 ~-8 glass'
Invoke-Chat '/tp @s ~7 ~8 ~-8 0 90'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'curve-above' | Out-Null
# close-up from just outside the middle of the turn
Invoke-Chat '/tp @s ~2 ~-8 ~10 180 45'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'curve-close' | Out-Null

if ($withMod) { Wait-LogLine 'tracks: rendering \d+ curve' 20 -Since $mark | Out-Null }

$r = Invoke-SelfTest 'tracks.*'
if ($r.Pass -lt 8) { throw "expected at least 8 tracks selftests to pass, got $($r.Pass)" }
