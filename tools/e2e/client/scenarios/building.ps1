# Building blocks (U10) on a real client: casings, windows, glass, a door, stairs, a slab,
# scaffolding, a girder, a placard, a table cloth and bars are set in front of the player, then
# the door is opened by redstone.
# Screenshots: out/building/{built,door_open}.png
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

$r = Invoke-SelfTest 'building.*'
if ($r.Pass -lt 15) { throw "expected at least 15 building selftests to pass, got $($r.Pass)" }
