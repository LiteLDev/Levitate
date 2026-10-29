# World item renderer (U33) on the client.
#   local / remote-mod: the client mod draws the display items of a chute feeding a moving belt
#                       into a depot, a smart chute and a brass funnel with filters, a placard
#                       holding a diamond and a deployer holding a sword (set by right-clicks), a
#                       mechanical crafter and a basin fed by chutes, and the /create itemrender
#                       test pattern (facing, lying, spinning, sliding).
#                       Belt / depot items are drawn by the mod only (no vanilla item entities).
#   remote-vanilla:     the same blocks; the belt / depot / chute items are item entities, the rest
#                       is not drawn (vanilla fallback), without errors.
# Screenshots: out/itemrender/{overview,belt-a,belt-b,filters,placard,deployer,crafter,basin,pattern-a,pattern-b}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

if (-not $vanilla -and $script:Mode -eq 'local') {
    $r = Invoke-SelfTest 'itemrender.*'
    if ($r.Pass -lt 8) { throw "expected at least 8 itemrender selftests to pass, got $($r.Pass)" }
}

Reset-CameraPerspective
Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/tp @s 7 -60 0 0 20'
Start-Sleep -Milliseconds 800
$cmds = @(
    '/fill -1 -60 4 19 -56 8 air',
    # chest -> chute -> belt along +x (driven from its start) -> depot
    '/setblock 0 -60 5 levitate:kinetics_test_source ["levitate:axis"=2]',
    '/setblock 0 -60 6 levitate:belt ["levitate:facing"=3,"levitate:part"=0]',
    '/fill 1 -60 6 3 -60 6 levitate:belt ["levitate:facing"=3,"levitate:part"=1]',
    '/setblock 4 -60 6 levitate:belt ["levitate:facing"=3,"levitate:part"=2]',
    '/setblock 5 -60 6 levitate:depot',
    '/setblock 0 -59 6 levitate:chute',
    '/setblock 0 -58 6 minecraft:chest',
    # filters, placard, deployer
    '/setblock 7 -60 6 levitate:smart_chute',
    '/setblock 9 -60 6 levitate:brass_funnel ["levitate:facing"=1]',
    '/setblock 11 -59 7 minecraft:stone',
    '/setblock 11 -59 6 levitate:placard ["levitate:facing"=2]',
    '/setblock 13 -59 6 levitate:deployer ["levitate:facing"=2]',
    # a crafter and a basin, each fed by a chest and a chute above it
    '/setblock 15 -60 6 levitate:mechanical_crafter ["levitate:facing"=2,"levitate:pointing"=0]',
    '/setblock 15 -59 6 levitate:chute',
    '/setblock 15 -58 6 minecraft:chest',
    '/setblock 17 -60 6 levitate:basin',
    '/setblock 17 -59 6 levitate:chute',
    '/setblock 17 -58 6 minecraft:chest'
)
foreach ($c in $cmds) { Invoke-Chat $c; Start-Sleep -Milliseconds 200 }
Start-Sleep -Seconds 2

function Use-On([string]$item, [double]$fx, [double]$fy, [double]$fz, [string]$logPattern) {
    Invoke-Chat '/clear @s'
    Invoke-Chat "/give @s $item 1"
    $sx = [math]::Floor($fx) + 0.5
    Invoke-Chat "/tp @s $sx -60 3.5 facing $fx $fy $fz"
    Start-Sleep -Milliseconds 700
    $mark = Get-LogMark
    Click 0.5 0.5 -Relative -Right
    Wait-LogLine $logPattern 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 300
}
Use-On 'diamond' 7.5 -59.3125 5.99 'filter slot at .* set to minecraft:diamond'
Use-On 'iron_ingot' 9.5 -59.875 5.99 'filter slot at .* set to minecraft:iron_ingot'
Use-On 'diamond' 11.5 -58.5 6.85 'building: placard at 11 -59 6 holds minecraft:diamond'
Use-On 'iron_sword' 13.5 -58.5 5.99 'deployer at .* now holds minecraft:iron_sword'
Invoke-Chat '/clear @s'

# items start to flow: chest -> chute -> belt -> depot
Invoke-Chat '/replaceitem block 0 -58 6 slot.container 0 iron_ingot 64'
Invoke-Chat '/replaceitem block 0 -58 6 slot.container 1 cobblestone 64'
Invoke-Chat '/replaceitem block 15 -58 6 slot.container 0 cobblestone 1'
Invoke-Chat '/replaceitem block 17 -58 6 slot.container 0 apple 3'
Invoke-Chat '/replaceitem block 17 -58 6 slot.container 1 cobblestone 16'
Invoke-Chat '/create kinetics 0 -60 5 speed -32'
if (-not $vanilla) {
    Wait-LogLine 'itemrender client: drawing \d+ display items' 30 | Out-Null
    if ($script:Mode -eq 'remote-mod') { Wait-LogLine 'itemrender: .* gets display items' 30 | Out-Null }
}
Invoke-Chat '/tp @s 8.5 -57 -1 facing 8.5 -60 6'
Start-Sleep -Seconds 4
Save-Screenshot 'overview' | Out-Null

Invoke-Chat '/tp @s 2.5 -59 3.5 facing 2.5 -60.3 6.5'
Start-Sleep -Seconds 1
Save-Screenshot 'belt-a' | Out-Null
Start-Sleep -Milliseconds 250
Save-Screenshot 'belt-b' | Out-Null

Invoke-Chat '/tp @s 8 -59.5 3.8 facing 8 -59.6 6'
Start-Sleep -Milliseconds 800
Save-Screenshot 'filters' | Out-Null
Invoke-Chat '/tp @s 11.5 -59.4 4.3 facing 11.5 -58.5 6.8'
Start-Sleep -Milliseconds 800
Save-Screenshot 'placard' | Out-Null
Invoke-Chat '/tp @s 13.5 -59.4 3.6 facing 13.5 -58.5 5.5'
Start-Sleep -Milliseconds 800
Save-Screenshot 'deployer' | Out-Null
Invoke-Chat '/tp @s 15.5 -59.6 4.2 facing 15.5 -59.5 6'
Start-Sleep -Milliseconds 800
Save-Screenshot 'crafter' | Out-Null
Invoke-Chat '/tp @s 17.5 -58.2 4.6 facing 17.5 -59.8 6.5'
Start-Sleep -Milliseconds 800
Save-Screenshot 'basin' | Out-Null

# the test pattern (display items without a block): facing, lying + piles, spinning, sliding
Invoke-Chat '/fill 0 -61 10 12 -61 18 smooth_stone'
Invoke-Chat '/create itemrender demo 2 -60 10'
Invoke-Chat '/tp @s 7.5 -58 7 facing 7.5 -60 14'
Start-Sleep -Seconds 2
Save-Screenshot 'pattern-a' | Out-Null
Start-Sleep -Milliseconds 400
Save-Screenshot 'pattern-b' | Out-Null
Invoke-Chat '/create itemrender clear'
Invoke-Chat '/create kinetics 0 -60 5 speed 0'
Assert-NoErrors
