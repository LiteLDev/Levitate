# Building blocks (U10) on a real client: casings, windows, glass, a door, stairs, a slab,
# scaffolding, a girder, a placard, a table cloth and bars are set in front of the player, then
# the door is opened by redstone (U43: drawn sliding by the client mod).
# U43: copycats get materials by real right-clicks (oak planks on a step, stone bricks on a panel,
# an oak log turned by a second click), a metal bracket goes on a shaft, an L of casings shows the
# inner corner of Create's connected textures, framed glass panes join.
#   local / remote-mod: the client mod draws the copycats in their materials, the bracket, the
#                       corner and the sliding door.
#   remote-vanilla:     the copycat base models and the plain shaft (no client mod), same server logic.
# Screenshots: out/building/{built,door_open,door_moving,copycats,ct}.png
$vanilla = $script:Mode -eq 'remote-vanilla'
if (-not $vanilla) {
    Wait-LogLine 'companion: server accepted the handshake' 90 | Out-Null
}
Reset-CameraPerspective
Start-Sleep -Milliseconds 500
Invoke-Chat '/tp @s 0 -60 -5 0 15'
Start-Sleep -Milliseconds 1500
$cmds = @(
    '/fill -4 -60 0 4 -56 1 air',
    '/setblock -4 -60 0 levitate:andesite_casing', '/setblock -3 -60 0 levitate:andesite_casing',
    '/setblock -4 -59 0 levitate:andesite_casing', '/setblock -3 -59 0 levitate:brass_casing',
    '/setblock -2 -60 0 levitate:oak_window', '/setblock -2 -59 0 levitate:oak_window',
    '/setblock -1 -60 0 levitate:framed_glass_pane', '/setblock -1 -59 0 levitate:framed_glass',
    '/setblock 0 -60 0 levitate:train_door ["levitate:facing"=2]',
    '/setblock 0 -59 0 levitate:train_door ["levitate:facing"=2,"levitate:upper"=1]',
    '/setblock 1 -60 0 levitate:copper_shingle_stairs ["levitate:facing"=3]',
    '/setblock 1 -59 0 levitate:copper_tile_slab',
    '/setblock 2 -60 0 levitate:copper_scaffolding', '/setblock 2 -59 0 levitate:copper_scaffolding',
    '/setblock 3 -60 0 levitate:metal_girder', '/setblock 3 -59 0 levitate:placard ["levitate:facing"=2]',
    '/setblock 4 -60 0 levitate:red_table_cloth', '/setblock 4 -59 0 levitate:copper_bars'
)
foreach ($c in $cmds) { Invoke-Chat $c; Start-Sleep -Milliseconds 250 }
Start-Sleep -Milliseconds 2500
Save-Screenshot 'built' | Out-Null
$mark = Get-LogMark
Invoke-Chat '/setblock 0 -60 1 redstone_block'
Wait-LogLine 'building: levitate:train_door at 0 -60 0 opened by redstone' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1500
Save-Screenshot 'door_open' | Out-Null
Invoke-Chat '/setblock 0 -60 1 air'
Wait-LogLine 'building: levitate:train_door at 0 -60 0 closed by redstone' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1200

# ---- U43: the sliding door moving (Create: 0.15 per tick, about 7 ticks) --------------------------
Invoke-Chat '/tp @s 0.5 -60 -2.5 0 20.5'   # looking south at the lower half (eyes 1.62 up)
Start-Sleep -Milliseconds 800
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'building: levitate:train_door at 0 -60 0 opened' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 120
Save-Screenshot 'door_moving' | Out-Null
Start-Sleep -Milliseconds 1200

# ---- U43: copycats, a bracket, connected-texture corners, panes ------------------------------------
$scene = @(
    '/fill -5 -60 3 5 -57 8 air',
    '/setblock -3 -60 4 levitate:copycat_step ["levitate:facing"=2]',
    '/setblock -1 -60 4 levitate:copycat_panel ["levitate:facing"=2]',
    '/setblock 1 -60 4 levitate:copycat_panel ["levitate:facing"=2]',
    '/setblock 3 -59 4 levitate:shaft ["levitate:axis"=0]',
    '/setblock 2 -59 4 levitate:shaft ["levitate:axis"=0]',
    '/setblock 4 -59 4 levitate:shaft ["levitate:axis"=0]',
    # an L of casings: the inner corner of the front face shows the frame's corner
    '/setblock -4 -60 7 levitate:andesite_casing', '/setblock -3 -60 7 levitate:andesite_casing',
    '/setblock -4 -59 7 levitate:andesite_casing',
    '/setblock -2 -60 7 levitate:andesite_casing', '/setblock -1 -60 7 levitate:andesite_casing',
    '/setblock -2 -59 7 levitate:andesite_casing', '/setblock -1 -59 7 levitate:andesite_casing',
    # framed glass panes, 2 x 2
    '/setblock 1 -60 7 levitate:framed_glass_pane', '/setblock 2 -60 7 levitate:framed_glass_pane',
    '/setblock 1 -59 7 levitate:framed_glass_pane', '/setblock 2 -59 7 levitate:framed_glass_pane'
)
foreach ($c in $scene) { Invoke-Chat $c; Start-Sleep -Milliseconds 250 }
Invoke-Chat '/clear @s'
Invoke-Chat '/give @s oak_planks 4'
Invoke-Chat '/give @s stone_bricks 4'
Invoke-Chat '/give @s oak_log 4'
Invoke-Chat '/give @s levitate:metal_bracket 2'
Start-Sleep -Milliseconds 800

function Use-At([double]$x, [double]$y, [double]$z, [string]$slot, [string]$expect) {
    # look south at the block centre from 3 blocks north (eyes at -58.38)
    $pitch = [Math]::Round([Math]::Atan2(-58.38 - ($y + 0.4), 2.6) * 180 / [Math]::PI, 1)
    Invoke-Chat "/tp @s $($x + 0.5) -60 $($z - 2.5) 0 $pitch"
    Start-Sleep -Milliseconds 700
    Press-Key $slot
    Start-Sleep -Milliseconds 400
    $m = Get-LogMark
    Click 0.5 0.5 -Relative -Right
    Wait-LogLine $expect 20 -Since $m | Out-Null
    Start-Sleep -Milliseconds 400
}
Use-At -3 -60 4 '1' 'building: levitate:copycat_step at -3 -60 4 takes the look of minecraft:oak_planks'
Use-At -1 -60 4 '2' 'building: levitate:copycat_panel at -1 -60 4 takes the look of minecraft:stone_bricks'
Use-At 1 -60 4 '3' 'building: levitate:copycat_panel at 1 -60 4 takes the look of minecraft:oak_log'
Use-At 1 -60 4 '3' 'building: levitate:copycat_panel at 1 -60 4 turned its material'
Use-At 3 -59 4 '4' 'building: levitate:metal_bracket applied to levitate:shaft at 3 -59 4'

Invoke-Chat '/tp @s 0.5 -59 1.5 0 22'
Start-Sleep -Milliseconds 2500
Save-Screenshot 'copycats' | Out-Null
Invoke-Chat '/tp @s -2 -59 4.5 0 15'
Start-Sleep -Milliseconds 2500
Save-Screenshot 'ct' | Out-Null
if (-not $vanilla) {
    $log = (Get-LeviLog) -join "`n"
    if ($log -notmatch 'building client: drawing') { throw 'the client mod drew no copycat / door / bracket' }
}

$r = Invoke-SelfTest 'building.*'
if ($r.Pass -lt 15) { throw "expected at least 15 building selftests to pass, got $($r.Pass)" }
