# Materials (U9) on a real client: textures and models of the ores, metal blocks, decorative stone
# (cube, pillar, stairs incl. corners, slabs incl. double, walls with connections) and item icons.
# U43: layered / pillar connected textures, tall wall sides, the block of experience at night (end
# rod particles: client mod locally, the server for vanilla clients), the chromatic compound's
# charge bar on the hotbar.
# Screenshots: out/materials/{blocks,palette,connected,items}.png
Reset-CameraPerspective
Start-Sleep -Milliseconds 500
Invoke-Chat '/tp @s ~ ~ ~ 0 20'
Start-Sleep -Milliseconds 800

# row 1 (z+4): material blocks
$row1 = 'zinc_ore', 'deepslate_zinc_ore', 'raw_zinc_block', 'zinc_block', 'brass_block', 'andesite_alloy_block',
    'industrial_iron_block', 'weathered_iron_block', 'rose_quartz_block', 'rose_quartz_tiles', 'experience_block', 'cardboard_block'
$x = -6
foreach ($b in $row1) { Invoke-Chat "/setblock ~$x ~ ~5 levitate:$b"; $x++ }
Start-Sleep -Milliseconds 1500
Save-Screenshot 'blocks' | Out-Null

# row 2 (z+3): one stone palette: raw, cut, polished, bricks, small bricks, layered, pillar, stairs, slabs, walls
foreach ($b in $row1) { Invoke-Chat "/setblock ~$(-6 + [array]::IndexOf($row1, $b)) ~ ~5 air" }
$row2 = 'asurine', 'cut_asurine', 'polished_cut_asurine', 'cut_asurine_bricks', 'small_asurine_bricks', 'layered_asurine', 'asurine_pillar'
$x = -6
foreach ($b in $row2) { Invoke-Chat "/setblock ~$x ~ ~5 levitate:$b"; $x++ }
Invoke-Chat '/setblock ~1 ~ ~5 levitate:cut_asurine_stairs ["minecraft:cardinal_direction"="south"]'
Invoke-Chat '/setblock ~2 ~ ~5 levitate:cut_asurine_slab'
Invoke-Chat '/setblock ~3 ~ ~5 levitate:cut_asurine_slab ["levitate:double"=true]'
Invoke-Chat '/setblock ~4 ~ ~5 levitate:cut_asurine_wall'
Invoke-Chat '/setblock ~5 ~ ~5 levitate:cut_asurine_wall'
Invoke-Chat '/setblock ~6 ~ ~5 levitate:crimsite'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'palette' | Out-Null

# U43: connected textures of layered stone and pillars, tall wall sides, the block of experience
foreach ($b in $row2) { Invoke-Chat "/setblock ~$(-6 + [array]::IndexOf($row2, $b)) ~ ~5 air" }
foreach ($c in @(
        '/fill ~-6 ~ ~5 ~6 ~3 ~6 air',
        '/fill ~-6 ~ ~5 ~-4 ~ ~5 levitate:layered_asurine', '/fill ~-6 ~1 ~5 ~-5 ~1 ~5 levitate:layered_asurine',
        '/fill ~-2 ~ ~5 ~-1 ~1 ~5 levitate:asurine_pillar', '/setblock ~0 ~ ~5 levitate:asurine_pillar',
        '/fill ~1 ~2 ~5 ~3 ~2 ~5 levitate:asurine_pillar ["minecraft:block_face"="east"]',
        '/fill ~1 ~ ~5 ~3 ~ ~5 levitate:cut_asurine_wall', '/setblock ~2 ~1 ~5 levitate:cut_asurine',
        '/setblock ~5 ~ ~5 levitate:experience_block')) { Invoke-Chat $c; Start-Sleep -Milliseconds 200 }
Invoke-Chat '/time set night'
Start-Sleep -Milliseconds 2500
Save-Screenshot 'connected' | Out-Null
Invoke-Chat '/time set noon'

# hotbar icons (+ U43: the charge bar of a chromatic compound with 6 of 10 charges)
Invoke-Chat '/clear @s'
foreach ($i in 'zinc_ingot', 'brass_ingot', 'andesite_alloy', 'precision_mechanism', 'electron_tube', 'builders_tea', 'blaze_cake', 'crushed_raw_iron', 'iron_sheet') {
    Invoke-Chat "/give @s levitate:$i 1"
}
Invoke-SelfTest 'materials.chromatic_bar_item' | Out-Null
Start-Sleep -Milliseconds 1200
Save-Screenshot 'items' | Out-Null

$r = Invoke-SelfTest 'materials.*'
if ($r.Pass -lt 10) { throw "expected at least 10 materials selftests to pass, got $($r.Pass)" }
