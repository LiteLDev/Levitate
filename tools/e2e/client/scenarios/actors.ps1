# Contraption actors (U22) on a real client.
#   local / remote-mod: the block models (harvester, plough, roller, PSI, controls, seats), a
#                       moving roller + harvester contraption (the client mod turns the drum and the
#                       blades), sitting down on a seat with a right-click; the selftests pass on the
#                       (integrated or BDS) server.
#   remote-vanilla:     the same models and moving blocks without the client mod (no animation).
# Screenshots: out/actors/{models,moving-a,moving-b,placed,seated}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

if (-not $vanilla) {
    $r = Invoke-SelfTest 'actors.*'
    if ($r.Pass -lt 11) { throw "expected at least 11 actors selftests to pass, got $($r.Pass)" }
}

Reset-CameraPerspective
Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/clear @s'
# look along +x at a row of blocks 5 blocks ahead
# a fixed spot (the test anchor takes absolute coordinates)
Invoke-Chat '/tp @s 0 -60 0 facing 10 -61 0'
Start-Sleep -Milliseconds 600
Invoke-Chat '/fill ~3 ~ ~-4 ~9 ~4 ~4 air'
Invoke-Chat '/setblock ~5 ~ ~-3 levitate:mechanical_harvester ["levitate:facing"=4]'
Invoke-Chat '/setblock ~5 ~ ~-2 levitate:mechanical_plough ["levitate:facing"=4]'
Invoke-Chat '/setblock ~5 ~ ~-1 levitate:mechanical_roller ["levitate:facing"=4]'
Invoke-Chat '/setblock ~5 ~ ~ levitate:portable_storage_interface ["levitate:facing"=4]'
Invoke-Chat '/setblock ~5 ~ ~1 levitate:contraption_controls ["levitate:facing"=4]'
Invoke-Chat '/setblock ~5 ~ ~2 levitate:red_seat'
Invoke-Chat '/setblock ~5 ~ ~3 levitate:light_blue_seat'
Start-Sleep -Seconds 2
Save-Screenshot 'models' | Out-Null

# a roller and a harvester hanging on slime blocks, moving towards the camera
Invoke-Chat '/fill ~5 ~ ~-3 ~5 ~ ~3 air'
Invoke-Chat '/setblock ~8 ~1 ~ slime'
Invoke-Chat '/setblock ~7 ~1 ~ levitate:mechanical_roller ["levitate:facing"=4]'
Invoke-Chat '/setblock ~8 ~1 ~1 slime'
Invoke-Chat '/setblock ~7 ~1 ~1 levitate:mechanical_harvester ["levitate:facing"=4]'
Invoke-Chat '/setblock ~8 ~1 ~-1 slime'
Start-Sleep -Milliseconds 800
$mark = Get-LogMark
Invoke-Chat '/create contraption assemble 8 -59 0'
$line = Wait-LogLine 'contraption #(\d+) assembled: (\d+) blocks' 30 -Since $mark
$id = [regex]::Match($line, 'contraption #(\d+) assembled').Groups[1].Value
Invoke-Chat "/create contraption move $id -3 0 0 120"
$animError = $null
if (-not $vanilla) {
    try { Wait-LogLine 'actors client: animating levitate:mechanical_' 30 -Since $mark | Out-Null } catch { $animError = $_ }
}
Start-Sleep -Milliseconds 1500
Save-Screenshot 'moving-a' | Out-Null
Start-Sleep -Milliseconds 300
Save-Screenshot 'moving-b' | Out-Null
Start-Sleep -Seconds 5
$mark = Get-LogMark
Invoke-Chat "/create contraption disassemble $id"
Wait-LogLine "contraption #$id disassembled" 30 -Since $mark | Out-Null
Start-Sleep -Seconds 1
Save-Screenshot 'placed' | Out-Null
Invoke-Chat '/fill ~3 ~ ~-4 ~9 ~4 ~4 air'

# sit down: right-click a seat right in front
Invoke-Chat '/setblock 2 -60 0 levitate:yellow_seat'
Invoke-Chat '/tp @s 0 -60 0.5 facing 2.5 -59.8 0.5'
Start-Sleep -Milliseconds 800
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'actors: minecraft:player sat down on the seat at' 20 -Since $mark | Out-Null
Start-Sleep -Seconds 1
Save-Screenshot 'seated' | Out-Null
Press-Key 'SHIFT' 400
Start-Sleep -Milliseconds 800
Invoke-Chat '/setblock 2 -60 0 air'
Assert-NoErrors
if ($animError) { throw $animError }
