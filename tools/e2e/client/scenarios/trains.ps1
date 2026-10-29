# Trains (U27) on a real client: a line east of the player curving north (the curve made with the
# track item, as a player does), `/create trains demo` puts station "Demo A" with a one-carriage
# train on it and station "Demo B" on the north branch; the train's schedule drives it through
# the curve to B. Then the station screen and the schedule editor.
#   local / remote-mod: native screens (client mod); remote-vanilla: the server forms
# Screenshots: out/trains/{assembled,moving-1,moving-2,moving-3,arrived,station-screen,schedule-screen}.png
$mark = Get-LogMark

Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/fill ~-4 ~ ~-34 ~34 ~5 ~5 air'
Invoke-Chat '/fill ~3 ~ ~ ~20 ~ ~ levitate:track ["levitate:shape"=2]'
Invoke-Chat '/fill ~29 ~ ~-9 ~29 ~ ~-32 levitate:track ["levitate:shape"=1]'
Invoke-Chat '/give @s levitate:track 64'
Press-Key 1
# select the east end of the line (x + 20), then click the north branch's south end (x + 29, z - 9)
Invoke-Chat '/tp @s ~17 ~ ~ -90 25'
Start-Sleep -Milliseconds 800
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'tracks: \S+ selected the track at' 20 -Since $mark | Out-Null
Invoke-Chat '/tp @s ~12 ~ ~-6 180 28'
Start-Sleep -Milliseconds 800
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'tracks: \S+ connected .* with a curve of length' 20 -Since $mark | Out-Null

# back to the start and build the stations and the train
Invoke-Chat '/tp @s ~-29 ~ ~6 -90 0'
Start-Sleep -Milliseconds 600
Invoke-Chat '/create trains demo'
Wait-LogLine "trains: assembled train #\d+ '.*' with 1 carriages" 20 -Since $mark | Out-Null
# watch from south-west of the curve
Invoke-Chat '/tp @s ~12 ~5 ~8 -135 25'
Start-Sleep -Milliseconds 800
Save-Screenshot 'assembled' | Out-Null
Wait-LogLine "trains: train #\d+ '.*' heading to station 'Demo B'" 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 2500
Save-Screenshot 'moving-1' | Out-Null
Start-Sleep -Milliseconds 1500
Save-Screenshot 'moving-2' | Out-Null
Start-Sleep -Milliseconds 1500
Save-Screenshot 'moving-3' | Out-Null
Wait-LogLine "trains: train #\d+ '.*' arrived at station 'Demo B'" 60 -Since $mark | Out-Null
Invoke-Chat '/tp @s ~14 ~ ~-22 -45 30'
Start-Sleep -Milliseconds 1200
Save-Screenshot 'arrived' | Out-Null

# the station screen (native with the client mod, a form without)
Invoke-Chat '/create trains screen'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'station-screen' | Out-Null
Close-Screen

# the schedule editor: right-click in the air with a schedule
Invoke-Chat '/give @s levitate:schedule 1'
Press-Key 2
Invoke-Chat '/tp @s ~ ~ ~ 0 -60'
Start-Sleep -Milliseconds 600
Click 0.5 0.5 -Relative -Right
Start-Sleep -Milliseconds 1500
Save-Screenshot 'schedule-screen' | Out-Null
Close-Screen

$r = Invoke-SelfTest 'trains.*' -TimeoutSeconds 900
if ($r.Pass -lt 6) { throw "expected at least 6 trains selftests to pass, got $($r.Pass)" }
