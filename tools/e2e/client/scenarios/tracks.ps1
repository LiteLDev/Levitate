# Tracks (U5) on a real client: place two tracks, connect them with a curve by right-clicking
# with the track item (select the first end, click the second), and screenshot the curve.
#   local / remote-mod: the client mod previews the placement before the second click and draws
#                       the curve (log "tracks: rendering ...")
#   remote-vanilla:     only the straight tracks and the track bed (fake tracks) are visible
# U48 (R18): a flat track between two sloped curves (built with the track item) tilts:
#   local / remote-mod: the client mod draws the block turned onto the ramp (log "tracks: drawing
#                       ... tilted track"), the curves meet it at its ends
#   remote-vanilla:     the flat block model (the curves and trains follow the tilt on the server)
# Screenshots: out/tracks/{placed,selected,preview,curve,curve-above,curve-close,tilt-side,tilt-close}.png
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
if ($withMod) {
    # U44: Create's placement preview (rails of the planned curve, "Can Connect" in the action bar)
    Wait-LogLine 'tracks: previewing a track placement \(valid\)' 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 600
    Save-Screenshot 'preview' | Out-Null
}
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

# ---- U48: tilted track between two sloped curves (absolute place east of the spawn) ----
# flat track M at (210, -56, 40), a slope rising away east of it, one rising towards it from the west
Invoke-Teleport '/tp @s 200 -60 44'
Invoke-Chat '/fill 195 -60 34 225 -44 46 air'
Invoke-Chat '/setblock 210 -56 40 levitate:track ["levitate:shape"=2]'
Invoke-Chat '/setblock 215 -54 40 levitate:track ["levitate:shape"=7]'
Invoke-Chat '/setblock 216 -53 40 levitate:track ["levitate:shape"=7]'
Invoke-Chat '/setblock 205 -59 40 levitate:track ["levitate:shape"=7]'
Invoke-Chat '/setblock 204 -60 40 levitate:track ["levitate:shape"=7]'
# glass to stand on at every place the player is put (it would fall otherwise)
foreach ($g in '207 -56 42', '213 -53 43', '213 -56 42', '207 -59 43', '210 -56 47', '211 -54 43') {
    Invoke-Chat "/setblock $g glass"
}
Invoke-Chat '/give @s levitate:track 64'
Press-Key 1
# (`facing` aims from the feet: the points are 1.62 below what the crosshair is on)
# the east end of M, then the upper slope: a sloped curve
Invoke-Teleport '/tp @s 207 -55 42 facing 210.5 -57.57 40.5'
$tm = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'tracks: \S+ selected the track at 210,-56,40' 20 -Since $tm | Out-Null
Invoke-Teleport '/tp @s 213 -52 43 facing 215.5 -55.12 40.5'
$tm = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'tracks: \S+ connected 210,-56,40 to 215,-54,40 with a curve' 20 -Since $tm | Out-Null
# the west end of M, then the lower slope: the second sloped curve tilts M
Invoke-Teleport '/tp @s 213 -55 42 facing 210.5 -57.57 40.5'
$tm = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'tracks: \S+ selected the track at 210,-56,40' 20 -Since $tm | Out-Null
Invoke-Teleport '/tp @s 207 -58 43 facing 205.5 -60.12 40.5'
$tm = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'tracks: \S+ connected 210,-56,40 to 205,-59,40 with a curve' 20 -Since $tm | Out-Null
Wait-LogLine 'tracks: tilted the track at 210,-56,40 by' 20 -Since $tm | Out-Null
Start-Sleep -Milliseconds 1500
# from the south, level with the ramp, and close up from above
Invoke-Teleport '/tp @s 210.5 -55 47 facing 210.5 -57.12 40.5'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'tilt-side' | Out-Null
Invoke-Teleport '/tp @s 211.5 -53 43 facing 210.5 -57.22 40.5'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'tilt-close' | Out-Null
if ($withMod) { Wait-LogLine 'tracks: drawing \d+ tilted track' 20 -Since $mark | Out-Null }

$r = Invoke-SelfTest 'tracks.*'
if ($r.Pass -lt 8) { throw "expected at least 8 tracks selftests to pass, got $($r.Pass)" }
