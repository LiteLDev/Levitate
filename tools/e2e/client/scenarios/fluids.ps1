# Fluid machines (U19) on a real client: the blocks' models and textures, a hose pulley turned
# by a test source lowering its hose (drawn by the client mod), a spout over a depot fed from a
# cauldron of water through a pump filling the bottles put on the depot, and the unit's selftests.
#   local / remote-mod: the client mod draws the hose (log "fluids: rendering ...") and the spout's
#                       stream with its nozzle squeezing (log "fluids: spout nozzle squeezes ...")
#   remote-vanilla:     static blocks; the spout's drops and splash (particles) and the filling
# Screenshots: out/fluids/{blocks,hose,spout-a,spout-b,spout-c}.png
# a chat line typed while the game is busy can get lost (remote clients more often): send the speed
# command again, up to three times, until the generator reports it
function Set-TestSpeed([string]$command) {
    $m = Get-LogMark
    Invoke-Chat $command
    foreach ($attempt in 1..3) {
        try { Wait-LogLine 'kinetics: Generator at .* now turns at 64 RPM' 10 -Since $m | Out-Null; return } catch {
            Write-Step "no speed change after '$command', sending it again"
            Invoke-Chat $command
        }
    }
    Wait-LogLine 'kinetics: Generator at .* now turns at 64 RPM' 20 -Since $m | Out-Null
}
$mark = Get-LogMark
$withMod = $script:Mode -ne 'remote-vanilla'

# content errors about our packs (fluids.prelaunch.ps1 turned the content log file on)
$logDir = Join-Path (Get-GameDataDir) 'logs'
$contentLog = Get-ChildItem $logDir -Filter 'ContentLog*' -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $script:FluidsContentLogStart } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($contentLog) {
    $errors = Select-String -Path $contentLog.FullName -Pattern 'levitate:(spout|item_drain|hose_pulley|portable_fluid_interface|honey|chocolate|fluids_test_depot)|spout.geo|item_drain.geo|hose_pulley.geo|fluid_source.geo|honey_bucket|chocolate_bucket' | ForEach-Object { $_.Line }
    if ($errors) {
        $errors | Set-Content (Join-Path (Join-Path $script:OutRoot $script:Unit) 'content-errors.txt')
        throw "content log reports $(@($errors).Count) problem(s) with the Levitate packs (content-errors.txt): $($errors | Select-Object -First 3)"
    }
}

Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/fill ~-4 ~ ~1 ~4 ~8 ~8 air'
# a row of blocks 4 blocks south of the player
Invoke-Chat '/setblock ~-2 ~ ~4 levitate:spout'
Invoke-Chat '/setblock ~-1 ~ ~4 levitate:item_drain'
Invoke-Chat '/setblock ~ ~ ~4 levitate:portable_fluid_interface ["levitate:facing"=2]'
Invoke-Chat '/setblock ~1 ~ ~4 levitate:honey'
Invoke-Chat '/setblock ~2 ~ ~4 levitate:chocolate'
# a hose pulley facing the player, turned by a kinetics test source (16 RPM)
Invoke-Chat '/setblock ~ ~5 ~6 levitate:hose_pulley ["levitate:facing"=2]'
Invoke-Chat '/setblock ~-1 ~5 ~6 levitate:kinetics_test_source ["levitate:axis"=0]'
Invoke-Chat '/give @s levitate:honey_bucket 1'
Invoke-Chat '/tp @s ~ ~1 ~ 0 20'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'blocks' | Out-Null

# the hose goes down 1/32 block per tick at 16 RPM: about 3 blocks in 5 s
Start-Sleep -Seconds 6
Invoke-Chat '/tp @s ~ ~ ~ 0 0'
Start-Sleep -Milliseconds 1000
Save-Screenshot 'hose' | Out-Null
if ($withMod) { Wait-LogLine 'fluids: rendering \d+ quads' 20 -Since $mark | Out-Null }

# a spout over a depot north of the player, fed from a cauldron of water by a pump (U17) east of it
Invoke-Chat '/fill ~-1 ~ ~-7 ~9 ~4 ~-2 air'
Invoke-Chat '/setblock ~3 ~ ~-4 levitate:depot'
Invoke-Chat '/setblock ~3 ~2 ~-4 levitate:spout'
Invoke-Chat '/setblock ~4 ~2 ~-4 levitate:fluid_pipe ["levitate:east"=1,"levitate:west"=1]'
Invoke-Chat '/setblock ~6 ~2 ~-4 levitate:fluid_pipe ["levitate:east"=1,"levitate:west"=1]'
Invoke-Chat '/setblock ~7 ~2 ~-4 cauldron ["fill_level"=6,"cauldron_liquid"="water"]'
Invoke-Chat '/setblock ~6 ~2 ~-3 levitate:kinetics_test_source ["levitate:axis"=0]'
Invoke-Chat '/setblock ~5 ~2 ~-3 levitate:kinetics_test_cog ["levitate:axis"=0]'
if ($withMod) { Set-TestSpeed '/create kinetics ~6 ~2 ~-3 speed 64' }
Start-Sleep -Milliseconds 500
Invoke-Chat '/setblock ~5 ~2 ~-4 levitate:mechanical_pump ["levitate:facing"=4]'
# the cauldron's bucket goes into the spout (1000 mB: four bottles); facing coordinates count from
# the eyes: aim at the depot's middle
Start-Sleep -Seconds 4
Invoke-Chat '/replaceitem entity @s slot.weapon.mainhand 0 glass_bottle 4'
Invoke-Chat '/tp @s ~ ~ ~ facing ~3 ~-1.2 ~-4'
Start-Sleep -Milliseconds 800
$spoutMark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Start-Sleep -Milliseconds 700
Save-Screenshot 'spout-a' | Out-Null
Start-Sleep -Milliseconds 250
Save-Screenshot 'spout-b' | Out-Null
Wait-LogLine 'spout at -?\d+ -?\d+ -?\d+ filled minecraft:glass_bottle into minecraft:potion' 20 -Since $spoutMark | Out-Null
Start-Sleep -Milliseconds 300
Save-Screenshot 'spout-c' | Out-Null
if ($withMod) { Wait-LogLine 'fluids: spout nozzle squeezes while filling' 20 -Since $mark | Out-Null }

$r = Invoke-SelfTest 'fluids.*'
if ($r.Pass -lt 14) { throw "expected at least 14 fluids selftests to pass, got $($r.Pass)" }
