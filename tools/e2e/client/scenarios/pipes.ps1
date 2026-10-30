# Fluid transport (U17) on the client: a cauldron of water pumped through glass pipes out of an open
# end next to the player.
#   local / remote-mod: the pump's cog turns (client mod), the client mod draws the water's flow
#                       fronts in the glass pipes (log "pipes client: drawing N glass pipe flows"),
#                       the open end pours fluid particles and places a water source; the selftests
#                       pass on the (integrated) server.
#   remote-vanilla:     a client without the mod sees the same static models and the water in the
#                       glass pipes (block states), without errors.
# Screenshots: out/pipes/{pipes-built,pipes-front (mod),pipes-flowing}.png
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
$vanilla = $script:Mode -eq 'remote-vanilla'

Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1 ~'
Start-Sleep -Milliseconds 500
# a line along z eight blocks ahead (out of reach of the water the open end pours, which would push
# the player and shift every relative position after it): test generator and cog first, set to 64 RPM
# before any fluid moves, then cauldron, pipe, pump (arrow south, turned by the cog on its east side),
# two glass pipes, pipe, open end
Invoke-Chat '/setblock ~9 ~1 ~-2 levitate:kinetics_test_source ["levitate:axis"=2]'
Invoke-Chat '/setblock ~9 ~1 ~-1 levitate:kinetics_test_cog ["levitate:axis"=2]'
# (like the kinetics scenario, not from a vanilla client: the generator keeps its default speed there)
if (-not $vanilla) { Set-TestSpeed '/create kinetics ~9 ~1 ~-2 speed 64' }
$rpm = if ($vanilla) { '\d+' } else { '64' }
Start-Sleep -Milliseconds 500
$mark = Get-LogMark
Invoke-Chat '/setblock ~8 ~1 ~-3 cauldron ["fill_level"=6,"cauldron_liquid"="water"]'
Invoke-Chat '/setblock ~8 ~1 ~-2 levitate:fluid_pipe ["levitate:north"=1,"levitate:south"=1]'
Invoke-Chat '/setblock ~8 ~1 ~0 levitate:glass_fluid_pipe ["levitate:axis"=2]'
Invoke-Chat '/setblock ~8 ~1 ~1 levitate:glass_fluid_pipe ["levitate:axis"=2]'
Invoke-Chat '/setblock ~8 ~1 ~2 levitate:fluid_pipe ["levitate:north"=1,"levitate:south"=1]'
# a fluid pipe junction and a smart pipe next to it for the models
Invoke-Chat '/setblock ~7 ~1 ~-4 levitate:fluid_pipe ["levitate:east"=1,"levitate:up"=1,"levitate:west"=1]'
Invoke-Chat '/setblock ~7 ~1 ~4 levitate:smart_fluid_pipe ["levitate:face"=0,"levitate:facing"=3]'
Invoke-Chat '/setblock ~7 ~2 ~4 levitate:fluid_valve ["levitate:facing"=1]'
Start-Sleep -Seconds 1
Save-Screenshot 'pipes-built' | Out-Null
# the pump last: it starts pumping as soon as it is placed
Invoke-Chat '/setblock ~8 ~1 ~-1 levitate:mechanical_pump ["levitate:facing"=3]'
Wait-LogLine "pipes: pump at .* now runs at $rpm RPM" 30 -Since $mark | Out-Null
if (-not $vanilla) {
    Wait-LogLine 'pipes client: drawing \d+ glass pipe flows' 30 -Since $mark | Out-Null
    Save-Screenshot 'pipes-front' | Out-Null # the cauldron's bucket is still on its way
}
Start-Sleep -Seconds 6
Save-Screenshot 'pipes-flowing' | Out-Null
Invoke-Chat '/create pipes ~8 ~1 ~0'
Wait-LogLine 'pipes: levitate:glass_fluid_pipe at' 30 -Since $mark | Out-Null

if (-not $vanilla) {
    $r = Invoke-SelfTest 'pipes.*'
    if ($r.Pass -lt 18) { throw "expected at least 18 pipes selftests to pass, got $($r.Pass)" }
}
Assert-NoErrors
