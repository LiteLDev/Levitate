# Trains (U27) on a real client: a line east of the player curving north (the curve made with the
# track item, as a player does), `/create trains demo` puts station "Demo A" with a one-carriage
# train on it and station "Demo B" on the north branch; the train's schedule drives it through
# the curve to B. Then the station screen and the schedule editor.
#   local / remote-mod: native screens (client mod); remote-vanilla: the server forms
# Screenshots: out/trains/{assembled,moving-1..5,arrived,station-screen,schedule-screen}.png
$mark = Get-LogMark

Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/fill ~-4 ~ ~-34 ~34 ~5 ~5 air'
# the two curve ends first (the player must not stand on track to click), the runs afterwards
Invoke-Chat '/setblock ~20 ~ ~ levitate:track ["levitate:shape"=2]'
Invoke-Chat '/setblock ~29 ~ ~-9 levitate:track ["levitate:shape"=1]'
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
Invoke-Chat '/fill ~-26 ~ ~6 ~-10 ~ ~6 levitate:track ["levitate:shape"=2]'
Invoke-Chat '/fill ~ ~ ~-4 ~ ~ ~-26 levitate:track ["levitate:shape"=1]'
Start-Sleep -Milliseconds 500

# back to the start and build the stations and the train
Invoke-Chat '/tp @s ~-29 ~ ~6 -90 0'
Start-Sleep -Milliseconds 600
Invoke-Chat '/create trains demo'
Wait-LogLine "trains: assembled train #\d+ '.*' with 1 carriages" 20 -Since $mark | Out-Null
# watch from just south-west of the curve
Invoke-Chat '/tp @s ~17 ~5 ~4 -135 32'
Start-Sleep -Milliseconds 600
Save-Screenshot 'assembled' | Out-Null
Wait-LogLine "trains: train #\d+ '.*' heading to station 'Demo B'" 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1800
foreach ($i in 1..5) {
    Save-Screenshot "moving-$i" | Out-Null
    Start-Sleep -Milliseconds 700
}
Wait-LogLine "trains: train #\d+ '.*' arrived at station 'Demo B'" 60 -Since $mark | Out-Null
Invoke-Chat '/tp @s ~9 ~2 ~-22 -135 30'
Start-Sleep -Milliseconds 1200
Save-Screenshot 'arrived' | Out-Null

# the station screen (native with the client mod, a form without)
Invoke-Chat '/create trains screen'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'station-screen' | Out-Null
# native screens close with the inventory key, forms with Esc
if ($script:Mode -eq 'remote-vanilla') { Close-Screen } else { Press-Key E; Start-Sleep -Milliseconds 800 }

# the schedule editor: right-click in the air with a schedule
Invoke-Chat '/give @s levitate:schedule 1'
Press-Key 2
Invoke-Chat '/tp @s ~ ~ ~ 0 -60'
Start-Sleep -Milliseconds 600
Click 0.5 0.5 -Relative -Right
Start-Sleep -Milliseconds 1500
Save-Screenshot 'schedule-screen' | Out-Null
if ($script:Mode -eq 'remote-vanilla') { Close-Screen } else { Press-Key E; Start-Sleep -Milliseconds 800 }

$r = Invoke-SelfTest 'trains.*' -TimeoutSeconds 900
if ($r.Pass -lt 7) { throw "expected at least 7 trains selftests to pass, got $($r.Pass)" }
