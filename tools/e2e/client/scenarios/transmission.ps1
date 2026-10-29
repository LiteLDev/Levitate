# Transmission (U6) on a real client: every block of the unit in a wall in front of the player,
# a driven line with both gauges, and the sequenced gearshift screen opened by right-clicking it.
#   local / remote-mod: the client mod spins the rotating parts and draws the eased gauge needles
#                       (log "transmission client: drawing N gauge needles"); right-clicking the
#                       sequenced gearshift opens the native instruction screen
#   remote-vanilla:     static models (needles at their 22.5 degree steps) and the form fallback
# Screenshots: out/transmission/{wall,wall-b,gauges,sequencer-screen}.png
$withMod = $script:Mode -ne 'remote-vanilla'
$mark    = Get-LogMark

# content errors about our blocks / models / textures (transmission.prelaunch.ps1 turned the log on)
$logDir = Join-Path (Get-GameDataDir) 'logs'
$contentLog = Get-ChildItem $logDir -Filter 'ContentLog*' -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $script:TransmissionContentLogStart } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($contentLog) {
    $pattern = 'transmission|levitate:(shaft|cogwheel|large_cogwheel|gearbox|vertical_gearbox|clutch|gearshift|encased_chain_drive|adjustable_chain_gearshift|andesite_encased|brass_encased|sequenced_gearshift|rotation_speed_controller|speedometer|stressometer)'
    $errors = Select-String -Path $contentLog.FullName -Pattern $pattern | ForEach-Object { $_.Line }
    if ($errors) {
        $errors | Set-Content (Join-Path (Join-Path $script:OutRoot $script:Unit) 'content-errors.txt')
        throw "content log reports $(@($errors).Count) problem(s) with the transmission blocks (content-errors.txt): $($errors | Select-Object -First 3)"
    }
}

if ($withMod -and $script:Mode -eq 'local') {
    $r = Invoke-SelfTest 'transmission.*'
    if ($r.Pass -lt 10) { throw "expected at least 10 transmission selftests to pass, got $($r.Pass)" }
}

Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/fill ~-2 ~ ~-8 ~12 ~6 ~8 air'
# look east at a wall 6 blocks ahead
Invoke-Chat '/tp @s ~ ~ ~ -90 10'
Start-Sleep -Milliseconds 500

# row 1 (y+1): shafts and cogs along x, driven by a source behind them
Invoke-Chat '/setblock ~7 ~1 ~-6 levitate:kinetics_test_source ["levitate:axis"=0]'
Invoke-Chat '/setblock ~6 ~1 ~-6 levitate:shaft ["levitate:axis"=0]'
Invoke-Chat '/setblock ~7 ~1 ~-4 levitate:kinetics_test_source ["levitate:axis"=0]'
Invoke-Chat '/setblock ~6 ~1 ~-4 levitate:cogwheel ["levitate:axis"=0]'
Invoke-Chat '/setblock ~6 ~2 ~-4 levitate:cogwheel ["levitate:axis"=0]'
Invoke-Chat '/setblock ~7 ~1 ~-2 levitate:kinetics_test_source ["levitate:axis"=0]'
Invoke-Chat '/setblock ~6 ~1 ~-2 levitate:large_cogwheel ["levitate:axis"=0]'
Invoke-Chat '/setblock ~7 ~1 ~1 levitate:kinetics_test_source ["levitate:axis"=0]'
Invoke-Chat '/setblock ~6 ~1 ~1 levitate:gearbox ["levitate:axis"=1]'
Invoke-Chat '/setblock ~5 ~1 ~1 levitate:shaft ["levitate:axis"=0]'
Invoke-Chat '/setblock ~6 ~1 ~2 levitate:shaft ["levitate:axis"=2]'
Invoke-Chat '/setblock ~7 ~1 ~4 levitate:kinetics_test_source ["levitate:axis"=0]'
Invoke-Chat '/setblock ~6 ~1 ~4 levitate:gearshift ["levitate:axis"=0,"levitate:powered"=1]'
Invoke-Chat '/setblock ~5 ~1 ~4 levitate:shaft ["levitate:axis"=0]'
Invoke-Chat '/setblock ~7 ~1 ~6 levitate:kinetics_test_source ["levitate:axis"=0]'
Invoke-Chat '/setblock ~6 ~1 ~6 levitate:clutch ["levitate:axis"=0]'
# row 2 (y+3): encased blocks, chain drives, sequenced gearshift, controller
Invoke-Chat '/setblock ~6 ~3 ~-6 levitate:andesite_encased_shaft ["levitate:axis"=0]'
Invoke-Chat '/setblock ~6 ~3 ~-5 levitate:brass_encased_shaft ["levitate:axis"=1]'
Invoke-Chat '/setblock ~6 ~3 ~-3 levitate:andesite_encased_cogwheel ["levitate:axis"=0,"levitate:top_shaft"=1]'
Invoke-Chat '/setblock ~6 ~3 ~-1 levitate:brass_encased_large_cogwheel ["levitate:axis"=0,"levitate:bottom_shaft"=1]'
Invoke-Chat '/setblock ~6 ~3 ~1 levitate:encased_chain_drive ["levitate:axis"=0]'
Invoke-Chat '/setblock ~6 ~3 ~2 levitate:adjustable_chain_gearshift ["levitate:axis"=0]'
Invoke-Chat '/setblock ~6 ~3 ~4 levitate:sequenced_gearshift ["levitate:axis"=0]'
Invoke-Chat '/setblock ~6 ~3 ~6 levitate:rotation_speed_controller ["levitate:axis"=2]'
Invoke-Chat '/setblock ~6 ~4 ~6 levitate:large_cogwheel ["levitate:axis"=0]'
Start-Sleep -Seconds 2
if ($withMod) {
    Invoke-Chat '/create kinetics ~7 ~1 ~-6 speed 32'
    Wait-LogLine 'kinetics client: drawing \d+ rotating parts' 30 | Out-Null
}
Start-Sleep -Seconds 2
Save-Screenshot 'wall' | Out-Null
Start-Sleep -Milliseconds 400
Save-Screenshot 'wall-b' | Out-Null

# at eye level: a driven line with both gauges facing the player (west), 3 blocks ahead
Invoke-Chat '/setblock ~3 ~1 ~-3 levitate:kinetics_test_source ["levitate:axis"=2]'
Invoke-Chat '/setblock ~3 ~1 ~-2 levitate:speedometer ["levitate:facing"=4,"levitate:along_first"=0]'
Invoke-Chat '/setblock ~3 ~1 ~-1 levitate:shaft ["levitate:axis"=2]'
Invoke-Chat '/setblock ~3 ~1 ~0 levitate:stressometer ["levitate:facing"=4,"levitate:along_first"=0]'
Invoke-Chat '/setblock ~3 ~1 ~1 levitate:kinetics_test_consumer ["levitate:axis"=2]'
# a sequenced gearshift on the ground 2 blocks ahead
Invoke-Chat '/setblock ~2 ~ ~ levitate:sequenced_gearshift ["levitate:axis"=0]'
Invoke-Chat '/tp @s ~ ~ ~ -90 10'
if ($withMod) { Wait-LogLine 'transmission client: drawing \d+ gauge needles' 30 | Out-Null }
Start-Sleep -Seconds 2
Save-Screenshot 'gauges' | Out-Null

# right-click the sequenced gearshift (with an item in hand: the game only sends block clicks
# with an empty hand for interactive blocks; Create opens the screen with any item but the wrench)
Invoke-Chat '/replaceitem entity @s slot.weapon.mainhand 0 stick'
Invoke-Chat '/tp @s ~ ~ ~ -90 35'
Start-Sleep -Milliseconds 800
$mark2 = Get-LogMark
Click 0.5 0.5 -Relative -Right
if ($withMod) {
    Wait-LogLine 'screen transmission.sequenced_gearshift opened for .* \(native\)' 20 -Since $mark2 | Out-Null
} else {
    Wait-LogLine 'screen transmission.sequenced_gearshift opened for .* \(form\)' 20 -Since $mark2 | Out-Null
}
Start-Sleep -Milliseconds 1200
Save-Screenshot 'sequencer-screen' | Out-Null
Close-Screen
if ($withMod) {
    Wait-LogLine 'transmission: sequenced gearshift at .* configured' 20 -Since $mark2 | Out-Null
}
Assert-NoErrors
