# Materials (U9) on a real client: textures and models of the ores, metal blocks, decorative stone
# (cube, pillar, stairs incl. corners, slabs incl. double, walls with connections) and item icons.
# Screenshots: out/materials/{blocks,palette,items}.png
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

# hotbar icons
Invoke-Chat '/clear @s'
foreach ($i in 'zinc_ingot', 'brass_ingot', 'andesite_alloy', 'precision_mechanism', 'electron_tube', 'builders_tea', 'blaze_cake', 'crushed_raw_iron', 'iron_sheet') {
    Invoke-Chat "/give @s levitate:$i 1"
}
Start-Sleep -Milliseconds 1200
Save-Screenshot 'items' | Out-Null

$r = Invoke-SelfTest 'materials.*'
if ($r.Pass -lt 10) { throw "expected at least 10 materials selftests to pass, got $($r.Pass)" }
