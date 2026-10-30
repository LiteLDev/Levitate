# U38 on a real client: standing on a lifted contraption (collision through the game's own shapes:
# the client mod's prediction, the server for vanilla clients) and relocating a train with the
# wrench (Create's TrainRelocator; the client mod draws the carriage at the target).
#   local / remote-mod: client mod; remote-vanilla: action-bar prompts only
# Screenshots: out/contraption_gaps/{on-contraption,relocate-selected,relocate-preview,relocated}.png
$mark = Get-LogMark
Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/fill ~-3 ~ ~-6 ~24 ~8 ~6 air'

# a lifted structure: the demo structure (stone, gold, diamond + two glued blocks) 4 blocks up
Invoke-Chat '/tp @s ~ ~ ~ -90 0'
Start-Sleep -Milliseconds 500
Invoke-Chat '/create contraption demo'
$line = Wait-LogLine 'contraption #(\d+) assembled: 5 blocks \(test\) at (-?\d+) (-?\d+) (-?\d+)' 20 -Since $mark
$m  = [regex]::Match($line, 'contraption #(\d+) assembled: 5 blocks \(test\) at (-?\d+) (-?\d+) (-?\d+)')
$id = $m.Groups[1].Value; $x = [int]$m.Groups[2].Value; $y = [int]$m.Groups[3].Value; $z = [int]$m.Groups[4].Value
Invoke-Chat "/create contraption move $id 0 4 0 20"
Start-Sleep -Seconds 2
# onto the lifted diamond block (y + 6), looking down: the player stays up there
Invoke-Chat "/tp @s $($x + 0.5) $($y + 7.2) $($z + 0.5) -90 70"
Start-Sleep -Seconds 3
Save-Screenshot 'on-contraption' | Out-Null
Invoke-Chat "/create contraption disassemble $id"
Wait-LogLine "contraption #$id disassembled" 20 -Since $mark | Out-Null
Invoke-Chat "/fill $($x - 1) $y $($z - 3) $($x + 1) $($y + 8) $($z + 3) air"
Invoke-Chat "/tp @s $($x - 4) $y $z -90 0"
Start-Sleep -Milliseconds 800

# a train to relocate: "Demo Manual" with its carriage at x + 7 (bogey), controls and a whistle
Invoke-Chat '/fill ~2 ~ ~ ~26 ~ ~ levitate:track ["levitate:shape"=2]'
Start-Sleep -Milliseconds 800
Invoke-Chat '/create trains demo_manual'
$tl = Wait-LogLine "trains: assembled train #(\d+) '.*' with 1 carriages" 20 -Since $mark
$train = [regex]::Match($tl, 'assembled train #(\d+)').Groups[1].Value
Invoke-Chat '/give @s levitate:wrench'
Start-Sleep -Milliseconds 500
Press-Key 1
# facing the carriage (controls at x + 7, y + 2) from the south
Invoke-Chat '/tp @s ~7 ~1 ~3 180 0'
Start-Sleep -Milliseconds 1000
Invoke-Chat '/replaceitem entity @s slot.weapon.mainhand 0 levitate:wrench'
Start-Sleep -Milliseconds 500
Click 0.5 0.5 -Relative -Right
Wait-LogLine "trains: \S+ is relocating train #\d+" 20 -Since $mark | Out-Null
Save-Screenshot 'relocate-selected' | Out-Null
# look at the track 7 blocks further east
Invoke-Chat '/tp @s ~7 ~ ~-1 180 36'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'relocate-preview' | Out-Null
if ($script:Mode -ne 'remote-vanilla') {
    Wait-LogLine 'trains client: drawing the relocation preview' 20 -Since $mark | Out-Null
}
Click 0.5 0.5 -Relative -Right
Wait-LogLine "trains: train #\d+ '.*' relocated by" 20 -Since $mark | Out-Null
Invoke-Chat '/tp @s ~ ~2 ~3 180 30'
Start-Sleep -Milliseconds 1200
Save-Screenshot 'relocated' | Out-Null
Invoke-Chat "/create killtrain $train"
