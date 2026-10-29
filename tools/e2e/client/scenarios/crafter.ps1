# Crafter unit (U16) on a real client: two mechanical crafters (the upper one points down into the
# lower one, which ejects out of its front), a saw facing up and a drill, all driven by test sources.
#   local / remote-mod: planks are put into the crafters by hand (right-click on the front), the
#                       client mod draws the slot items, the grid sliding down and the sticks
#                       growing; the saw blade and the drill head spin.
#   remote-vanilla:     the static models (blade / drill head in place), no errors.
# Screenshots: out/crafter/{built,filled,exporting,crafting,done,machines-a,machines-b}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

if (-not $vanilla) {
    $r = Invoke-SelfTest 'crafter.*'
    if ($r.Pass -lt 12) { throw "expected at least 12 crafter selftests to pass, got $($r.Pass)" }
}

Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1.6 ~'
Start-Sleep -Milliseconds 500
# crafters facing west (towards the player): A below, B above pointing down into A
Invoke-Chat '/setblock ~5 ~1 ~ levitate:mechanical_crafter ["levitate:facing"=4,"levitate:pointing"=2,"levitate:covered"=0]'
Invoke-Chat '/setblock ~5 ~2 ~ levitate:mechanical_crafter ["levitate:facing"=4,"levitate:pointing"=2,"levitate:covered"=0]'
Invoke-Chat '/setblock ~6 ~1 ~ levitate:kinetics_test_source ["levitate:axis"=0]'
# a saw facing up (blade along z) and a drill facing the player, each with a source
Invoke-Chat '/setblock ~4 ~ ~-2 levitate:mechanical_saw ["levitate:facing"=1,"levitate:axis_along_first"=0]'
Invoke-Chat '/setblock ~4 ~ ~-3 levitate:kinetics_test_source ["levitate:axis"=2]'
Invoke-Chat '/setblock ~5 ~1 ~3 levitate:mechanical_drill ["levitate:facing"=4]'
Invoke-Chat '/setblock ~6 ~1 ~3 levitate:kinetics_test_source ["levitate:axis"=0]'
Invoke-Chat '/setblock ~5 ~2 ~-2 levitate:mechanical_saw ["levitate:facing"=4,"levitate:axis_along_first"=1]'
Invoke-Chat '/setblock ~5 ~1 ~-2 levitate:kinetics_test_source ["levitate:axis"=1]'
Start-Sleep -Seconds 2
Save-Screenshot 'built' | Out-Null

if (-not $vanilla) {
    # put a plank into each crafter by hand
    Invoke-Chat '/replaceitem entity @s slot.weapon.mainhand 0 oak_planks 8'
    $mark = Get-LogMark
    Invoke-Chat '/tp @s ~ ~ ~ facing ~5 ~1.5 ~0.5'
    Start-Sleep -Milliseconds 600
    Click 0.5 0.5 -Relative -Right
    Wait-LogLine 'crafter at .* inserted minecraft:oak_planks' 15 -Since $mark | Out-Null
    $mark = Get-LogMark
    Invoke-Chat '/tp @s ~ ~ ~ facing ~5 ~2.5 ~0.5'
    Start-Sleep -Milliseconds 600
    Click 0.5 0.5 -Relative -Right
    Wait-LogLine 'crafter at .* inserted minecraft:oak_planks' 15 -Since $mark | Out-Null
    Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1.6 ~'
    Start-Sleep -Milliseconds 800
    Wait-LogLine 'crafter client: drawing items of \d+ crafters' 20 | Out-Null
    Save-Screenshot 'filled' | Out-Null
}

# power everything (slow: the animation takes a few seconds)
$mark = Get-LogMark
Invoke-Chat '/create kinetics ~6 ~1 ~ speed 16'
Invoke-Chat '/create kinetics ~4 ~ ~-3 speed 32'
Invoke-Chat '/create kinetics ~6 ~1 ~3 speed 32'
Invoke-Chat '/create kinetics ~5 ~1 ~-2 speed 32'
if (-not $vanilla) {
    Wait-LogLine 'crafter at .*: crafting started with 2 crafters' 30 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 1800
    Save-Screenshot 'exporting' | Out-Null
    Wait-LogLine 'crafter at .* crafted minecraft:stick x4' 30 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 2500
    Save-Screenshot 'crafting' | Out-Null
    Start-Sleep -Seconds 5
    Save-Screenshot 'done' | Out-Null
}
Start-Sleep -Seconds 1
Invoke-Chat '/tp @s ~ ~ ~ facing ~5 ~1 ~0'
Start-Sleep -Milliseconds 800
Save-Screenshot 'machines-a' | Out-Null
Start-Sleep -Milliseconds 300
Save-Screenshot 'machines-b' | Out-Null
Assert-NoErrors
