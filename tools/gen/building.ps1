# Generates the data files of the building unit (U10): behavior pack blocks, loot tables and
# recipes, resource pack geometry and fragments, processing recipes and pending recipes.
#
#   pwsh tools/gen/building.ps1
#
# Textures are drawn by tools/textures/building.ps1; block names live in
# assets/pack/rp/texts.d/<locale>/building.lang (hand written, Create's wording).
#
# Conventions shared with src/feature/building (keep them in sync):
#   levitate:facing      0..5 = down, up, north, south, west, east (Java Direction order)
#   horizontal "facing"  2..5 of a 0..5 state (Bedrock stores list states by index: lists must
#                        be 0..n); a model is drawn for facing north and turned clockwise
#   levitate:c_<dir>     connected-texture neighbours (casings, framed glass, windows)
#   levitate:north/..    pane / bars arms
#   Block-space boxes are [x0,y0,z0,x1,y1,z1] in pixels (0..16), as Create's VoxelShapes.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$bp = Join-Path $root 'assets/pack/bp'
$rp = Join-Path $root 'assets/pack/rp'
$FormatVersion = '1.21.90'

# Bedrock block models are drawn with the X axis mirrored compared to world space.
$MirrorX = $true

# ---- catalog --------------------------------------------------------------------------------------
$Woods = @('oak', 'spruce', 'birch', 'jungle', 'acacia', 'dark_oak', 'mangrove', 'crimson', 'warped', 'cherry', 'bamboo')
$WindowTypes = $Woods + @('ornate_iron', 'industrial_iron', 'weathered_iron')
$Colours = @('white', 'orange', 'magenta', 'light_blue', 'yellow', 'lime', 'pink', 'gray', 'light_gray', 'cyan', 'purple', 'blue', 'brown', 'green', 'red', 'black')
$Stages = @('', 'exposed_', 'weathered_', 'oxidized_')
$StageTex = @('fresh', 'exposed', 'weathered', 'oxidized')
$Casings = [ordered]@{
    andesite_casing         = @{ sound = 'wood'; map = '#a3a593'; hard = 2.0; light = 0 }
    brass_casing            = @{ sound = 'wood'; map = '#c08d3e'; hard = 2.0; light = 0 }
    copper_casing           = @{ sound = 'copper'; map = '#b5694d'; hard = 2.0; light = 0 }
    shadow_steel_casing     = @{ sound = 'stone'; map = '#3b3644'; hard = 2.0; light = 0 }
    refined_radiance_casing = @{ sound = 'stone'; map = '#e9e5d4'; hard = 2.0; light = 12 }
    railway_casing          = @{ sound = 'netherite'; map = '#4d4a55'; hard = 2.0; light = 0 }
}
$GlassKinds = [ordered]@{ framed_glass = 'omni'; horizontal_framed_glass = 'horizontal'; vertical_framed_glass = 'vertical'; tiled_glass = 'none' }
$Metals = @('andesite', 'brass', 'copper')
$WoodenDoors = @('minecraft:wooden_door', 'minecraft:spruce_door', 'minecraft:birch_door', 'minecraft:jungle_door', 'minecraft:acacia_door',
    'minecraft:dark_oak_door', 'minecraft:mangrove_door', 'minecraft:cherry_door', 'minecraft:bamboo_door', 'minecraft:crimson_door',
    'minecraft:warped_door', 'minecraft:pale_oak_door')
$WoodenTrapdoors = @('minecraft:trapdoor', 'minecraft:spruce_trapdoor', 'minecraft:birch_trapdoor', 'minecraft:jungle_trapdoor',
    'minecraft:acacia_trapdoor', 'minecraft:dark_oak_trapdoor', 'minecraft:mangrove_trapdoor', 'minecraft:cherry_trapdoor',
    'minecraft:bamboo_trapdoor', 'minecraft:crimson_trapdoor', 'minecraft:warped_trapdoor', 'minecraft:pale_oak_trapdoor')
# sliding doors: id -> texture, folds (Create: andesite and copper fold), sound, glass
$Doors = [ordered]@{
    andesite_door     = @{ folds = $true; sound = 'stone'; glass = $false; casing = 'andesite_casing' }
    brass_door        = @{ folds = $false; sound = 'stone'; glass = $false; casing = 'brass_casing' }
    copper_door       = @{ folds = $true; sound = 'stone'; glass = $false; casing = 'copper_casing' }
    train_door        = @{ folds = $false; sound = 'netherite'; glass = $false; casing = 'railway_casing' }
    framed_glass_door = @{ folds = $false; sound = 'glass'; glass = $true; casing = 'framed_glass' }
}
$Trapdoors = [ordered]@{
    train_trapdoor        = @{ sound = 'netherite'; glass = $false; casing = 'railway_casing' }
    framed_glass_trapdoor = @{ sound = 'glass'; glass = $true; casing = 'framed_glass' }
}

$Generated = [System.Collections.Generic.List[string]]::new()
$TerrainTextures = [ordered]@{}
$BlockSounds = [ordered]@{}

# ---- helpers --------------------------------------------------------------------------------------
function Write-Json([string]$rel, $obj) {
    $path = Join-Path $root $rel
    New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
    $json = $obj | ConvertTo-Json -Depth 64
    [System.IO.File]::WriteAllText($path, $json + "`n", [System.Text.UTF8Encoding]::new($false))
    $script:Generated.Add($rel.Replace('\', '/'))
}
function Tex([string]$name, [string]$file) {
    $key = "levitate_$name"
    if (-not $script:TerrainTextures.Contains($key)) { $script:TerrainTextures[$key] = [ordered]@{ textures = "textures/blocks/building/$file" } }
    return $key
}
function Q([string]$state) { "q.block_state('levitate:$state')" }
# a horizontal facing condition; down / up (the default 0) count as north
function HF([int]$f) { if ($f -eq 2) { return "$(Q facing) <= 2" } return "$(Q facing) == $f" }
function Mat([string]$tex, [string]$method = 'opaque', [bool]$ao = $true) {
    $m = [ordered]@{ texture = $tex; render_method = $method }
    if (-not $ao) { $m.ambient_occlusion = $false }
    return $m
}
function Box6([double[]]$b) {
    # Bedrock boxes: origin [-8..8] on x/z, size in pixels; clamp to the block cell
    $x0 = [Math]::Max(0, $b[0]); $y0 = [Math]::Max(0, $b[1]); $z0 = [Math]::Max(0, $b[2])
    $x1 = [Math]::Min(16, $b[3]); $y1 = [Math]::Min(16, $b[4]); $z1 = [Math]::Min(16, $b[5])
    if ($MirrorX) { $ox = 8 - $x1 } else { $ox = $x0 - 8 }
    return [ordered]@{ origin = @($ox, $y0, ($z0 - 8)); size = @(($x1 - $x0), ($y1 - $y0), ($z1 - $z0)) }
}
# rotate a block-space box clockwise (seen from above) by quarter turns: north -> east -> south -> west
function Rot-Y([double[]]$b, [int]$quarters) {
    $r = $b
    for ($i = 0; $i -lt (($quarters % 4) + 4) % 4; $i++) {
        $r = @((16 - $r[5]), $r[1], $r[0], (16 - $r[2]), $r[4], $r[3])
    }
    return , $r
}
# turn a box drawn for facing up into facing north (attached to the south)
function Up-To-North([double[]]$b) { return , @($b[0], $b[2], (16 - $b[4]), $b[3], $b[5], (16 - $b[1])) }
function Up-To-Down([double[]]$b) { return , @($b[0], (16 - $b[4]), $b[2], $b[3], (16 - $b[1]), $b[5]) }
# horizontal facing index (2 n, 3 s, 4 w, 5 e) -> clockwise quarter turns from north
$Quarter = @{ 2 = 0; 5 = 1; 3 = 2; 4 = 3 }
function Facing-Box([double[]]$up, [int]$facing) {
    switch ($facing) {
        0 { return , (Up-To-Down $up) }
        1 { return , $up }
        default { return , (Rot-Y (Up-To-North $up) $Quarter[$facing]) }
    }
}

# A cube of a geometry from a block-space box; UVs are projected (Java style) onto a 16x16 tile,
# offset by $uvOff (u, v) inside the texture.
function Cube([double[]]$b, [double[]]$uvOff = @(0, 0), [string[]]$faces = $null, [double]$inflate = 0) {
    $x0 = $b[0]; $y0 = $b[1]; $z0 = $b[2]; $x1 = $b[3]; $y1 = $b[4]; $z1 = $b[5]
    $dx = $x1 - $x0; $dy = $y1 - $y0; $dz = $z1 - $z0
    $u = $uvOff[0]; $v = $uvOff[1]
    function W([double]$a) { return [Math]::Min(16, [Math]::Max(0, $a)) }
    $uv = [ordered]@{
        north = [ordered]@{ uv = @(($u + (W (16 - $x1))), ($v + (W (16 - $y1)))); uv_size = @($dx, $dy) }
        south = [ordered]@{ uv = @(($u + (W $x0)), ($v + (W (16 - $y1)))); uv_size = @($dx, $dy) }
        east  = [ordered]@{ uv = @(($u + (W (16 - $z1))), ($v + (W (16 - $y1)))); uv_size = @($dz, $dy) }
        west  = [ordered]@{ uv = @(($u + (W $z0)), ($v + (W (16 - $y1)))); uv_size = @($dz, $dy) }
        up    = [ordered]@{ uv = @(($u + (W $x0)), ($v + (W $z0))); uv_size = @($dx, $dz) }
        down  = [ordered]@{ uv = @(($u + (W $x0)), ($v + (W (16 - $z1)))); uv_size = @($dx, $dz) }
    }
    if ($faces) { foreach ($k in @($uv.Keys)) { if ($faces -notcontains $k) { $uv.Remove($k) } } }
    if ($MirrorX) { $ox = 8 - $x1 } else { $ox = $x0 - 8 }
    $c = [ordered]@{ origin = @($ox, $y0, ($z0 - 8)); size = @($dx, $dy, $dz); uv = $uv }
    if ($inflate -ne 0) { $c.inflate = $inflate }
    return $c
}
function Bone([string]$name, $cubes) { return [ordered]@{ name = $name; pivot = @(0, 0, 0); cubes = @($cubes) } }
function Write-Geo([string]$name, $bones, [int]$tw = 16, [int]$th = 16, [string]$file = $null) {
    $geo = [ordered]@{
        format_version      = '1.16.0'
        'minecraft:geometry' = @([ordered]@{
                description = [ordered]@{
                    identifier = "geometry.levitate.$name"; texture_width = $tw; texture_height = $th
                    visible_bounds_width = 3; visible_bounds_height = 3; visible_bounds_offset = @(0, 0.5, 0)
                }
                bones       = @($bones)
            })
    }
    # rotor models are read by the kinetics client from models/blocks/ (not recursive)
    if (-not $file) { $file = "building/$name" }
    Write-Json "assets/pack/rp/models/blocks/$file.geo.json" $geo
    return "geometry.levitate.$name"
}

function Block-Json([string]$id, $states, $components, $permutations) {
    $desc = [ordered]@{ identifier = "levitate:$id"; menu_category = [ordered]@{ category = 'construction' } }
    if ($states -and $states.Count -gt 0) { $desc.states = $states }
    $block = [ordered]@{ description = $desc; components = $components }
    if ($permutations -and @($permutations).Count -gt 0) { $block.permutations = @($permutations) }
    Write-Json "assets/pack/bp/blocks/$id.json" ([ordered]@{ format_version = $FormatVersion; 'minecraft:block' = $block })
}
function Base-Components([string]$id, [double]$hard, [double]$blast, [string]$map, [string[]]$tools, [string]$loot = $null) {
    $c = [ordered]@{
        'minecraft:destructible_by_mining'    = [ordered]@{ seconds_to_destroy = $hard }
        'minecraft:destructible_by_explosion' = [ordered]@{ explosion_resistance = $blast }
        'minecraft:map_color'                 = $map
        'minecraft:loot'                      = $(if ($loot) { $loot } else { "loot_tables/blocks/$id.json" })
    }
    foreach ($t in $tools) { $c["tag:minecraft:is_${t}_item_destructible"] = [ordered]@{} }
    return $c
}
function Loot([string]$id, [string]$item = $null, [int]$count = 1, [string]$suffix = '') {
    if (-not $item) { $item = "levitate:$id" }
    $entry = [ordered]@{ type = 'item'; name = $item; weight = 1 }
    if ($count -ne 1) { $entry.functions = @([ordered]@{ function = 'set_count'; count = $count }) }
    Write-Json "assets/pack/bp/loot_tables/blocks/$id$suffix.json" ([ordered]@{ pools = @([ordered]@{ rolls = 1; entries = @($entry) }) })
}
function Loot-Silk([string]$id) {
    $pool = [ordered]@{ rolls = 1; conditions = @([ordered]@{ condition = 'match_tool'; enchantments = @([ordered]@{ enchantment = 'silk_touch'; levels = [ordered]@{ range_min = 1 } }) })
        entries = @([ordered]@{ type = 'item'; name = "levitate:$id"; weight = 1 }) }
    Write-Json "assets/pack/bp/loot_tables/blocks/$id.json" ([ordered]@{ pools = @($pool) })
}
function Bool-States([string[]]$names) { $s = [ordered]@{}; foreach ($n in $names) { $s["levitate:$n"] = @(0, 1) }; return $s }
function Sel([double[]]$b) { return Box6 $b }

# ---- connected textures -------------------------------------------------------------------------
# A face texture is chosen by which of its 4 edges continue into a neighbour of the same block:
# mask bits 1 = top, 2 = right, 4 = bottom, 8 = left (as seen from outside the face).
# For each face: the neighbour direction behind its top / right / bottom / left edge.
$FaceEdges = [ordered]@{
    north = @('up', 'west', 'down', 'east')
    south = @('up', 'east', 'down', 'west')
    east  = @('up', 'north', 'down', 'south')
    west  = @('up', 'south', 'down', 'north')
    up    = @('north', 'east', 'south', 'west')
    down  = @('south', 'east', 'north', 'west')
}
# (unit-cube faces are world faces: a texture reads unmirrored from outside the block)
$Dirs = @('down', 'up', 'north', 'south', 'west', 'east')

# omni: all edges connect; horizontal: left/right of side faces; vertical: top/bottom of side faces
function Ct-Mask([string]$face, [hashtable]$conn, [string]$kind) {
    $e = $FaceEdges[$face]; $m = 0
    $side = $face -notin @('up', 'down')
    for ($i = 0; $i -lt 4; $i++) {
        $use = switch ($kind) {
            'omni' { $true }
            'horizontal' { $side -and ($i -eq 1 -or $i -eq 3) }
            'vertical' { $side -and ($i -eq 0 -or $i -eq 2) }
            default { $false }
        }
        if ($use -and $conn[$e[$i]]) { $m = $m -bor (1 -shl $i) }
    }
    return $m
}
function Ct-Block([string]$id, [string]$kind, [string]$texBase, [string]$method, $comps, [string[]]$dirsUsed, [string]$endTex = $null) {
    $states = Bool-States ($dirsUsed | ForEach-Object { "c_$_" })
    $comps['minecraft:geometry'] = 'minecraft:geometry.full_block'
    $comps['minecraft:material_instances'] = [ordered]@{ '*' = (Mat (Tex "${texBase}_0" "${texBase}_0") $method) }
    $perms = [System.Collections.Generic.List[object]]::new()
    $n = $dirsUsed.Count
    for ($bits = 0; $bits -lt (1 -shl $n); $bits++) {
        $conn = @{}; $cond = @()
        for ($i = 0; $i -lt $n; $i++) {
            $on = (($bits -shr $i) -band 1) -eq 1
            $conn[$dirsUsed[$i]] = $on
            $cond += "$(Q "c_$($dirsUsed[$i])") == $([int]$on)"
        }
        $mi = [ordered]@{}
        foreach ($face in $FaceEdges.Keys) {
            if ($endTex -and $face -in @('up', 'down')) { $mi[$face] = Mat $endTex $method; continue }
            $mask = Ct-Mask $face $conn $kind
            $mi[$face] = Mat (Tex "${texBase}_$mask" "${texBase}_$mask") $method
        }
        $mi['*'] = $mi['north']
        $perms.Add([ordered]@{ condition = ($cond -join ' && '); components = [ordered]@{ 'minecraft:material_instances' = $mi } })
    }
    Block-Json $id $states $comps $perms
}

# ---- casings ------------------------------------------------------------------------------------------
foreach ($id in $Casings.Keys) {
    $c = $Casings[$id]
    $comps = Base-Components $id $c.hard 6 $c.map @('pickaxe', 'axe')
    if ($c.light -gt 0) { $comps['minecraft:light_emission'] = $c.light }
    Ct-Block $id 'omni' $id 'opaque' $comps $Dirs
    Loot $id
    $BlockSounds["levitate:$id"] = $c.sound
}

# ---- connected glass ---------------------------------------------------------------------------------
foreach ($id in $GlassKinds.Keys) {
    $kind = $GlassKinds[$id]
    $comps = Base-Components $id 0.3 0.3 '#c8dbe0' @()
    $comps['minecraft:light_dampening'] = 0
    switch ($kind) {
        'omni' { Ct-Block $id 'omni' $id 'alpha_test' $comps $Dirs }
        'horizontal' { Ct-Block $id 'horizontal' $id 'alpha_test' $comps @('north', 'south', 'west', 'east') (Tex "${id}_end" "${id}_end") }
        'vertical' { Ct-Block $id 'vertical' $id 'alpha_test' $comps @('down', 'up') (Tex "${id}_end" "${id}_end") }
        default {
            $comps['minecraft:geometry'] = 'minecraft:geometry.full_block'
            $comps['minecraft:material_instances'] = [ordered]@{ '*' = (Mat (Tex $id $id) 'alpha_test') }
            Block-Json $id $null $comps $null
        }
    }
    Loot-Silk $id # Create: connected glass drops only with silk touch
    $BlockSounds["levitate:$id"] = 'glass'
}
Write-Json 'assets/pack/bp/loot_tables/blocks/building_empty.json' ([ordered]@{ pools = @() })

# ---- windows (vertical connected textures) -----------------------------------------------------------
foreach ($w in $WindowTypes) {
    $id = "${w}_window"
    $comps = Base-Components $id 0.3 0.3 '#a9c3cf' @()
    $comps['minecraft:light_dampening'] = 0
    Ct-Block $id 'vertical' $id 'alpha_test' $comps @('down', 'up') (Tex "${id}_end" "${id}_end")
    Loot-Silk $id
    $BlockSounds["levitate:$id"] = 'glass'
}

# ---- panes and bars ---------------------------------------------------------------------------------------
$PaneGeo = Write-Geo 'pane' @(
    (Bone 'post' (Cube @(7, 0, 7, 9, 16, 9)))
    (Bone 'n' (Cube @(7, 0, 0, 9, 16, 7) @(0, 0) @('east', 'west', 'up', 'down')))
    (Bone 's' (Cube @(7, 0, 9, 9, 16, 16) @(0, 0) @('east', 'west', 'up', 'down')))
    (Bone 'w' (Cube @(0, 0, 7, 7, 16, 9) @(0, 0) @('north', 'south', 'up', 'down')))
    (Bone 'e' (Cube @(9, 0, 7, 16, 16, 9) @(0, 0) @('north', 'south', 'up', 'down')))
)
$BarsGeo = Write-Geo 'bars' @(
    (Bone 'post' (Cube @(7, 0, 7, 9, 16, 9)))
    (Bone 'n' (Cube @(8, 0, 0, 8, 16, 7) @(0, 0) @('east', 'west')))
    (Bone 's' (Cube @(8, 0, 9, 8, 16, 16) @(0, 0) @('east', 'west')))
    (Bone 'w' (Cube @(0, 0, 8, 7, 16, 8) @(0, 0) @('north', 'south')))
    (Bone 'e' (Cube @(9, 0, 8, 16, 16, 8) @(0, 0) @('north', 'south')))
)
function Pane-Block([string]$id, [string]$geo, [string]$tex, [double]$hard, [string]$map, [string[]]$tools, [string]$sound, [bool]$drops) {
    $comps = Base-Components $id $hard $hard $map $tools
    $comps['minecraft:light_dampening'] = 0
    $comps['minecraft:geometry'] = [ordered]@{
        identifier      = $geo
        bone_visibility = [ordered]@{ post = $true; n = "$(Q north) == 1"; s = "$(Q south) == 1"; w = "$(Q west) == 1"; e = "$(Q east) == 1" }
    }
    $comps['minecraft:material_instances'] = [ordered]@{ '*' = (Mat $tex 'alpha_test') }
    $comps['minecraft:collision_box'] = Box6 @(7, 0, 7, 9, 16, 9)
    $comps['minecraft:selection_box'] = Box6 @(7, 0, 7, 9, 16, 9)
    $perms = @(
        [ordered]@{ condition = "$(Q north) == 1 || $(Q south) == 1 || $(Q west) == 1 || $(Q east) == 1"; components = [ordered]@{ 'minecraft:selection_box' = (Box6 @(0, 0, 0, 16, 16, 16)) } }
    )
    Block-Json $id (Bool-States @('north', 'south', 'west', 'east')) $comps $perms
    if ($drops) { Loot $id } else { Loot-Silk $id }
    $script:BlockSounds["levitate:$id"] = $sound
}
foreach ($id in $GlassKinds.Keys) { Pane-Block "${id}_pane" $PaneGeo (Tex "${id}_pane" "${id}_pane") 0.3 '#c8dbe0' @() 'glass' $false }
foreach ($w in $WindowTypes) { Pane-Block "${w}_window_pane" $PaneGeo (Tex "${w}_window_pane" "${w}_window_pane") 0.3 '#a9c3cf' @() 'glass' $false }
foreach ($m in $Metals) { Pane-Block "${m}_bars" $BarsGeo (Tex "${m}_bars" "${m}_bars") 5 '#8d8f86' @('pickaxe') 'metal' $true }

# ---- copper shingles and tiles ------------------------------------------------------------------------------
# stairs: 8 octant bones; facing 2..5, levitate:upside, levitate:shape 0 straight, 1 inner_left,
# 2 inner_right, 3 outer_left, 4 outer_right (Java StairsShape order). Quadrants of the step half
# for facing north: nw ne sw se.
$StairGeo = Write-Geo 'stairs' @(
    (Bone 'lnw' (Cube @(0, 0, 0, 8, 8, 8))); (Bone 'lne' (Cube @(8, 0, 0, 16, 8, 8)))
    (Bone 'lsw' (Cube @(0, 0, 8, 8, 8, 16))); (Bone 'lse' (Cube @(8, 0, 8, 16, 8, 16)))
    (Bone 'unw' (Cube @(0, 8, 0, 8, 16, 8))); (Bone 'une' (Cube @(8, 8, 0, 16, 16, 8)))
    (Bone 'usw' (Cube @(0, 8, 8, 8, 16, 16))); (Bone 'use' (Cube @(8, 8, 8, 16, 16, 16)))
)
# quadrants (index 0 nw, 1 ne, 2 se, 3 sw: clockwise) of the step for facing north per shape
$StairQuads = @{ 0 = @(0, 1); 1 = @(0, 1, 3); 2 = @(0, 1, 2); 3 = @(0); 4 = @(1) }
$QuadName = @('nw', 'ne', 'se', 'sw')
$stairVis = @{}
foreach ($q in $QuadName) { $stairVis[$q] = [System.Collections.Generic.List[string]]::new() }
foreach ($f in 2, 3, 4, 5) {
    foreach ($s in 0..4) {
        foreach ($qi in $StairQuads[$s]) {
            $world = ($qi + $Quarter[$f]) % 4
            $stairVis[$QuadName[$world]].Add("($(HF $f) && $(Q shape) == $s)")
        }
    }
}
$StairBones = [ordered]@{}
foreach ($q in $QuadName) {
    $step = '(' + ($stairVis[$q] -join ' || ') + ')'
    $StairBones["l$q"] = "$(Q upside) == 1 || $step"
    $StairBones["u$q"] = "$(Q upside) == 0 || $step"
}
$SlabGeo = Write-Geo 'slab' @((Bone 'bottom' (Cube @(0, 0, 0, 16, 8, 16))), (Bone 'top' (Cube @(0, 8, 0, 16, 16, 16))))

$CopperIds = [System.Collections.Generic.List[string]]::new()
foreach ($type in @('shingles', 'tiles')) {
    $single = $type.TrimEnd('s')
    foreach ($waxed in @('', 'waxed_')) {
        for ($si = 0; $si -lt 4; $si++) {
            $stage = $Stages[$si]
            $tex = Tex "copper_${type}_$($StageTex[$si])" "copper_${type}_$($StageTex[$si])"
            $map = @('#c06a50', '#a8806a', '#6c9a7e', '#56a38e')[$si]
            # full block
            $id = "${waxed}${stage}copper_$type"
            $comps = Base-Components $id 3 6 $map @('pickaxe')
            $comps['minecraft:geometry'] = 'minecraft:geometry.full_block'
            $comps['minecraft:material_instances'] = [ordered]@{ '*' = (Mat $tex) }
            Block-Json $id $null $comps $null
            Loot $id; $CopperIds.Add($id)
            # stairs
            $sid = "${waxed}${stage}copper_${single}_stairs"
            $comps = Base-Components $sid 3 6 $map @('pickaxe')
            $comps['minecraft:geometry'] = [ordered]@{ identifier = $StairGeo; bone_visibility = $StairBones }
            $comps['minecraft:material_instances'] = [ordered]@{ '*' = (Mat $tex) }
            $states = [ordered]@{ 'levitate:facing' = @(0, 1, 2, 3, 4, 5); 'levitate:upside' = @(0, 1); 'levitate:shape' = @(0, 1, 2, 3, 4) }
            Block-Json $sid $states $comps $null
            Loot $sid; $CopperIds.Add($sid)
            # slab
            $lid = "${waxed}${stage}copper_${single}_slab"
            $comps = Base-Components $lid 3 6 $map @('pickaxe')
            $comps['minecraft:geometry'] = [ordered]@{ identifier = $SlabGeo; bone_visibility = [ordered]@{ bottom = "$(Q top) == 0 || $(Q double) == 1"; top = "$(Q top) == 1 || $(Q double) == 1" } }
            $comps['minecraft:material_instances'] = [ordered]@{ '*' = (Mat $tex) }
            $comps['minecraft:collision_box'] = Box6 @(0, 0, 0, 16, 8, 16)
            $comps['minecraft:selection_box'] = Box6 @(0, 0, 0, 16, 8, 16)
            $perms = @(
                [ordered]@{ condition = "$(Q top) == 1 && $(Q double) == 0"; components = [ordered]@{ 'minecraft:collision_box' = (Box6 @(0, 8, 0, 16, 16, 16)); 'minecraft:selection_box' = (Box6 @(0, 8, 0, 16, 16, 16)) } }
                [ordered]@{ condition = "$(Q double) == 1"; components = [ordered]@{ 'minecraft:collision_box' = (Box6 @(0, 0, 0, 16, 16, 16)); 'minecraft:selection_box' = (Box6 @(0, 0, 0, 16, 16, 16)); 'minecraft:loot' = "loot_tables/blocks/${lid}_double.json" } }
            )
            Block-Json $lid (Bool-States @('top', 'double')) $comps $perms
            Loot $lid; Loot $lid $null 2 '_double'; $CopperIds.Add($lid)
            foreach ($x in @($id, $sid, $lid)) { $BlockSounds["levitate:$x"] = 'copper' }
        }
    }
}

# ---- scaffolding, ladders --------------------------------------------------------------------------------------
$ScaffoldGeo = Write-Geo 'scaffolding' @(
    (Bone 'top' @((Cube @(0, 14, 0, 16, 16, 16) @(0, 0) @('up', 'down')), (Cube @(0, 14, 0, 16, 16, 16) @(0, 0) @('north', 'south', 'east', 'west'))))
    (Bone 'legs' @((Cube @(0, 0, 0, 2, 14, 2)), (Cube @(14, 0, 0, 16, 14, 2)), (Cube @(0, 0, 14, 2, 14, 16)), (Cube @(14, 0, 14, 16, 14, 16))))
    (Bone 'half' @((Cube @(0, 8, 0, 2, 14, 2)), (Cube @(14, 8, 0, 16, 14, 2)), (Cube @(0, 8, 14, 2, 14, 16)), (Cube @(14, 8, 14, 16, 14, 16)), (Cube @(0, 8, 0, 16, 10, 16) @(0, 0) @('up', 'down'))))
)
foreach ($m in $Metals) {
    $id = "${m}_scaffolding"
    $comps = Base-Components $id 1 3 '#8d8f86' @('pickaxe')
    $comps['minecraft:light_dampening'] = 0
    $comps['minecraft:geometry'] = [ordered]@{ identifier = $ScaffoldGeo; bone_visibility = [ordered]@{ top = $true; legs = "$(Q bottom) == 0"; half = "$(Q bottom) == 1" } }
    $comps['minecraft:material_instances'] = [ordered]@{ '*' = (Mat (Tex "${m}_scaffolding" "${m}_scaffolding") 'alpha_test') }
    $comps['minecraft:collision_box'] = Box6 @(0, 14, 0, 16, 16, 16)
    $states = [ordered]@{ 'levitate:bottom' = @(0, 1); 'levitate:distance' = @(0, 1, 2, 3, 4, 5, 6, 7) }
    Block-Json $id $states $comps $null
    Loot $id
    $BlockSounds["levitate:$id"] = 'scaffolding'
}
$LadderBones = @()
$LadderVis = [ordered]@{}
foreach ($f in 2, 3, 4, 5) {
    $q = $Quarter[$f]
    $cubes = @(
        (Cube (Rot-Y @(1, 0, 14, 3, 16, 16) $q)), (Cube (Rot-Y @(13, 0, 14, 3 + 12, 16, 16) $q)),
        (Cube (Rot-Y @(3, 2, 15, 13, 3, 16) $q)), (Cube (Rot-Y @(3, 6, 15, 13, 7, 16) $q)),
        (Cube (Rot-Y @(3, 10, 15, 13, 11, 16) $q)), (Cube (Rot-Y @(3, 14, 15, 13, 15, 16) $q))
    )
    $LadderBones += Bone "f$f" $cubes
    $LadderVis["f$f"] = "$(HF $f)"
}
$LadderGeo = Write-Geo 'ladder' $LadderBones
foreach ($m in $Metals) {
    $id = "${m}_ladder"
    $comps = Base-Components $id 0.4 0.4 '#8d8f86' @('pickaxe')
    $comps['minecraft:light_dampening'] = 0
    $comps['minecraft:geometry'] = [ordered]@{ identifier = $LadderGeo; bone_visibility = $LadderVis }
    $comps['minecraft:material_instances'] = [ordered]@{ '*' = (Mat (Tex "${m}_ladder" "${m}_ladder") 'alpha_test') }
    $perms = foreach ($f in 2, 3, 4, 5) {
        $b = Rot-Y @(0, 0, 13, 16, 16, 16) $Quarter[$f]
        [ordered]@{ condition = "$(HF $f)"; components = [ordered]@{ 'minecraft:collision_box' = (Box6 $b); 'minecraft:selection_box' = (Box6 $b) } }
    }
    Block-Json $id ([ordered]@{ 'levitate:facing' = @(0, 1, 2, 3, 4, 5) }) $comps $perms
    Loot $id
    $BlockSounds["levitate:$id"] = 'metal'
}

# ---- doors and trapdoors ------------------------------------------------------------------------------------------
# A door half is drawn from a 16x32 texture: upper half v 0..16, lower half v 16..32. Shapes for
# facing north (Create's SlidingDoorShapes); the open panel is clipped to Bedrock's model bounds.
function Door-Geo([string]$name, [bool]$folds) {
    $bones = @()
    $vis = [ordered]@{}
    foreach ($f in 2, 3, 4, 5) {
        $q = $Quarter[$f]
        foreach ($half in @('l', 'u')) {
            $v = if ($half -eq 'u') { 0 } else { 16 }
            $hu = if ($half -eq 'u') { 1 } else { 0 }
            $bones += Bone "c$half$f" (Cube (Rot-Y @(0, 0, 13, 16, 16, 16) $q) @(0, $v))
            $vis["c$half$f"] = "$(HF $f) && $(Q upper) == $hu && $(Q open) == 0"
            if ($folds) {
                # folded leaves (Create: 9 px deep against the hinge side)
                $bones += Bone "r$half$f" @((Cube (Rot-Y @(13, 0, 7, 16, 16, 16) $q) @(0, $v)), (Cube (Rot-Y @(16, 0, 7, 19, 16, 16) $q) @(8, $v)))
                $bones += Bone "o$half$f" @((Cube (Rot-Y @(0, 0, 7, 3, 16, 16) $q) @(0, $v)), (Cube (Rot-Y @(-3, 0, 7, 0, 16, 16) $q) @(8, $v)))
            } else {
                $bones += Bone "r$half$f" (Cube (Rot-Y @(13, 0, 13, 23, 16, 16) $q) @(0, $v))
                $bones += Bone "o$half$f" (Cube (Rot-Y @(-7, 0, 13, 3, 16, 16) $q) @(6, $v))
            }
            # hinge 1 (right) slides east for facing north, hinge 0 (left) slides west
            $vis["r$half$f"] = "$(HF $f) && $(Q upper) == $hu && $(Q open) == 1 && $(Q hinge) == 1"
            $vis["o$half$f"] = "$(HF $f) && $(Q upper) == $hu && $(Q open) == 1 && $(Q hinge) == 0"
        }
    }
    $g = Write-Geo $name $bones 16 32
    return @{ geo = $g; vis = $vis }
}
$SlideGeo = Door-Geo 'door_slide' $false
$FoldGeo = Door-Geo 'door_fold' $true
foreach ($id in $Doors.Keys) {
    $d = $Doors[$id]
    $g = if ($d.folds) { $FoldGeo } else { $SlideGeo }
    $comps = Base-Components $id 3 3 '#8d8f86' @('pickaxe')
    $comps['minecraft:light_dampening'] = 0
    $comps['minecraft:geometry'] = [ordered]@{ identifier = $g.geo; bone_visibility = $g.vis }
    $comps['minecraft:material_instances'] = [ordered]@{ '*' = (Mat (Tex $id $id) 'alpha_test') }
    $comps['minecraft:collision_box'] = Box6 @(0, 0, 0, 16, 16, 16)
    $perms = @([ordered]@{ condition = "$(Q upper) == 1"; components = [ordered]@{ 'minecraft:loot' = 'loot_tables/blocks/building_empty.json' } })
    $states = [ordered]@{ 'levitate:facing' = @(0, 1, 2, 3, 4, 5); 'levitate:upper' = @(0, 1); 'levitate:hinge' = @(0, 1); 'levitate:open' = @(0, 1); 'levitate:powered' = @(0, 1) }
    Block-Json $id $states $comps $perms
    Loot $id
    $BlockSounds["levitate:$id"] = $d.sound
}
$tdBones = @(); $tdVis = [ordered]@{}
foreach ($f in 2, 3, 4, 5) {
    $q = $Quarter[$f]
    $tdBones += Bone "b$f" (Cube (Rot-Y @(0, 0, 0, 16, 3, 16) $q))
    $tdBones += Bone "t$f" (Cube (Rot-Y @(0, 13, 0, 16, 16, 16) $q))
    $tdBones += Bone "o$f" (Cube (Rot-Y @(0, 0, 13, 16, 16, 16) $q))
    $tdVis["b$f"] = "$(HF $f) && $(Q open) == 0 && $(Q top) == 0"
    $tdVis["t$f"] = "$(HF $f) && $(Q open) == 0 && $(Q top) == 1"
    $tdVis["o$f"] = "$(HF $f) && $(Q open) == 1"
}
$TrapdoorGeo = Write-Geo 'trapdoor' $tdBones
foreach ($id in $Trapdoors.Keys) {
    $d = $Trapdoors[$id]
    $comps = Base-Components $id 3 3 '#8d8f86' @('pickaxe')
    $comps['minecraft:light_dampening'] = 0
    $comps['minecraft:geometry'] = [ordered]@{ identifier = $TrapdoorGeo; bone_visibility = $tdVis }
    $comps['minecraft:material_instances'] = [ordered]@{ '*' = (Mat (Tex $id $id) 'alpha_test') }
    $comps['minecraft:collision_box'] = Box6 @(0, 0, 0, 16, 3, 16)
    $perms = @(
        [ordered]@{ condition = "$(Q open) == 0 && $(Q top) == 1"; components = [ordered]@{ 'minecraft:collision_box' = (Box6 @(0, 13, 0, 16, 16, 16)) } }
    )
    foreach ($f in 2, 3, 4, 5) {
        $perms += [ordered]@{ condition = "$(Q open) == 1 && $(HF $f)"; components = [ordered]@{ 'minecraft:collision_box' = (Box6 (Rot-Y @(0, 0, 13, 16, 16, 16) $Quarter[$f])) } }
    }
    $states = [ordered]@{ 'levitate:facing' = @(0, 1, 2, 3, 4, 5); 'levitate:top' = @(0, 1); 'levitate:open' = @(0, 1); 'levitate:powered' = @(0, 1) }
    Block-Json $id $states $comps $perms
    Loot $id
    $BlockSounds["levitate:$id"] = $d.sound
}

# ---- girders ------------------------------------------------------------------------------------------------------------
$GirderGeo = Write-Geo 'metal_girder' @(
    (Bone 'pole' (Cube @(4, 0, 4, 12, 16, 12)))
    (Bone 'x' @((Cube @(0, 2, 4, 16, 4, 12)), (Cube @(0, 12, 4, 16, 14, 12)), (Cube @(0, 4, 7, 16, 12, 9) @(0, 0) @('north', 'south'))))
    (Bone 'z' @((Cube @(4, 2, 0, 12, 4, 16)), (Cube @(4, 12, 0, 12, 14, 16)), (Cube @(7, 4, 0, 9, 12, 16) @(0, 0) @('east', 'west'))))
    (Bone 'top' (Cube @(5, 14, 5, 11, 16, 11)))
    (Bone 'bottom' (Cube @(5, 0, 5, 11, 2, 11)))
)
$comps = Base-Components 'metal_girder' 2 6 '#5b5d63' @('pickaxe')
$comps['minecraft:light_dampening'] = 0
$comps['minecraft:geometry'] = [ordered]@{
    identifier      = $GirderGeo
    bone_visibility = [ordered]@{
        pole   = "$(Q axis) == 1 || ($(Q x) == 0 && $(Q z) == 0)"
        x      = "$(Q x) == 1"; z = "$(Q z) == 1"
        top    = "$(Q axis) != 1 && $(Q top) == 1"; bottom = "$(Q axis) != 1 && $(Q bottom) == 1"
    }
}
$comps['minecraft:material_instances'] = [ordered]@{ '*' = (Mat (Tex 'metal_girder' 'metal_girder') 'alpha_test') }
$comps['minecraft:collision_box'] = Box6 @(4, 0, 4, 12, 16, 12)
$states = [ordered]@{ 'levitate:axis' = @(0, 1, 2); 'levitate:x' = @(0, 1); 'levitate:z' = @(0, 1); 'levitate:top' = @(0, 1); 'levitate:bottom' = @(0, 1) }
$perms = @(
    [ordered]@{ condition = "$(Q x) == 1 || $(Q z) == 1"; components = [ordered]@{ 'minecraft:collision_box' = (Box6 @(0, 2, 0, 16, 14, 16)) } }
)
Block-Json 'metal_girder' $states $comps $perms
Loot 'metal_girder'
$BlockSounds['levitate:metal_girder'] = 'netherite'

# encased shaft: levitate:axis is the shaft axis (0 x, 2 z); the beam runs across it
$GirderShaftGeo = Write-Geo 'metal_girder_encased_shaft' @(
    (Bone 'beam_x' @((Cube @(0, 2, 4, 16, 4, 12)), (Cube @(0, 12, 4, 16, 14, 12)), (Cube @(0, 4, 4, 16, 12, 5) @(0, 0) @('north', 'south')), (Cube @(0, 4, 11, 16, 12, 12) @(0, 0) @('north', 'south'))))
    (Bone 'beam_z' @((Cube @(4, 2, 0, 12, 4, 16)), (Cube @(4, 12, 0, 12, 14, 16)), (Cube @(4, 4, 0, 5, 12, 16) @(0, 0) @('east', 'west')), (Cube @(11, 4, 0, 12, 12, 16) @(0, 0) @('east', 'west'))))
    (Bone 'shaft_x' (Cube @(0, 6, 6, 16, 10, 10) @(0, 0) $null))
    (Bone 'shaft_z' (Cube @(6, 6, 0, 10, 10, 16) @(0, 0) $null))
    (Bone 'top' (Cube @(5, 14, 5, 11, 16, 11)))
    (Bone 'bottom' (Cube @(5, 0, 5, 11, 2, 11)))
)
$RotorGeo = Write-Geo 'girder_shaft_rotor' @((Bone 'rotor' (Cube @(6, 0, 6, 10, 16, 10)))) 16 16 'building_girder_shaft_rotor'
$comps = Base-Components 'metal_girder_encased_shaft' 2 6 '#5b5d63' @('pickaxe') 'loot_tables/blocks/metal_girder_encased_shaft.json'
$comps['minecraft:light_dampening'] = 0
$comps['minecraft:geometry'] = [ordered]@{
    identifier      = $GirderShaftGeo
    bone_visibility = [ordered]@{
        beam_x = "$(Q axis) == 2"; beam_z = "$(Q axis) == 0"
        shaft_x = "$(Q axis) == 0 && $(Q rotor) == 0"; shaft_z = "$(Q axis) == 2 && $(Q rotor) == 0"
        top = "$(Q top) == 1"; bottom = "$(Q bottom) == 1"
    }
}
$comps['minecraft:material_instances'] = [ordered]@{ '*' = (Mat (Tex 'metal_girder' 'metal_girder') 'alpha_test') }
$comps['minecraft:collision_box'] = Box6 @(0, 2, 0, 16, 14, 16)
$states = [ordered]@{ 'levitate:axis' = @(0, 1, 2); 'levitate:top' = @(0, 1); 'levitate:bottom' = @(0, 1); 'levitate:rotor' = @(0, 1) }
Block-Json 'metal_girder_encased_shaft' $states $comps $null
# Create: the encased girder drops the girder and the shaft
Write-Json 'assets/pack/bp/loot_tables/blocks/metal_girder_encased_shaft.json' ([ordered]@{ pools = @(
            [ordered]@{ rolls = 1; entries = @([ordered]@{ type = 'item'; name = 'levitate:metal_girder'; weight = 1 }) }
            [ordered]@{ rolls = 1; entries = @([ordered]@{ type = 'item'; name = 'levitate:shaft'; weight = 1 }) }
        )
    })
$BlockSounds['levitate:metal_girder_encased_shaft'] = 'netherite'
$null = Tex 'girder_shaft' 'girder_shaft'

# ---- brackets (item icons; applied to shafts, cogs and pipes) -------------------------------------------------
$BracketGeo = Write-Geo 'bracket' @(
    (Bone 'plate' (Cube @(3, 0, 3, 13, 2, 13)))
    (Bone 'arms' @((Cube @(3, 2, 6, 5, 12, 10)), (Cube @(11, 2, 6, 13, 12, 10)), (Cube @(3, 10, 6, 13, 12, 10))))
)
foreach ($b in @('metal_bracket', 'wooden_bracket')) {
    $comps = Base-Components $b 1 3 '#7b6a4c' @('pickaxe', 'axe')
    $comps['minecraft:light_dampening'] = 0
    $comps['minecraft:geometry'] = $BracketGeo
    $comps['minecraft:material_instances'] = [ordered]@{ '*' = (Mat (Tex $b $b) 'alpha_test') }
    $comps['minecraft:collision_box'] = Box6 @(3, 0, 3, 13, 12, 13)
    $comps['minecraft:selection_box'] = Box6 @(3, 0, 3, 13, 12, 13)
    Block-Json $b $null $comps $null
    Loot $b
    $BlockSounds["levitate:$b"] = $(if ($b -eq 'wooden_bracket') { 'scaffolding' } else { 'netherite' })
}

# ---- table cloths --------------------------------------------------------------------------------------------------------
$ClothGeo = Write-Geo 'table_cloth' @(
    (Bone 'top' (Cube @(-1, 0, -1, 17, 1, 17) @(0, 0) @('up', 'down')))
    (Bone 'skirt' @(
            (Cube @(-1, -7, -1, 17, 1, -1) @(0, 0) @('north', 'south')), (Cube @(-1, -7, 17, 17, 1, 17) @(0, 0) @('north', 'south')),
            (Cube @(-1, -7, -1, -1, 1, 17) @(0, 0) @('east', 'west')), (Cube @(17, -7, -1, 17, 1, 17) @(0, 0) @('east', 'west'))))
)
foreach ($c in ($Colours + $Metals)) {
    $id = "${c}_table_cloth"
    $comps = Base-Components $id 0.8 0.8 '#d8d2c4' @()
    $comps['minecraft:light_dampening'] = 0
    $comps['minecraft:geometry'] = $ClothGeo
    $comps['minecraft:material_instances'] = [ordered]@{ '*' = (Mat (Tex $id $id) 'alpha_test') }
    $comps['minecraft:collision_box'] = Box6 @(0, 0, 0, 16, 1, 16)
    $comps['minecraft:selection_box'] = Box6 @(0, 0, 0, 16, 1, 16)
    Block-Json $id $null $comps $null
    Loot $id
    $BlockSounds["levitate:$id"] = $(if ($c -in $Metals) { 'metal' } else { 'cloth' })
}

# ---- placard ---------------------------------------------------------------------------------------------------------------
$plBones = @(); $plVis = [ordered]@{}; $plPerms = @()
foreach ($f in 0..5) {
    $b = Facing-Box @(2, 0, 2, 14, 3, 14) $f
    $plBones += Bone "f$f" (Cube $b)
    $plVis["f$f"] = "$(Q facing) == $f"
    $plPerms += [ordered]@{ condition = "$(Q facing) == $f"; components = [ordered]@{ 'minecraft:collision_box' = (Box6 $b); 'minecraft:selection_box' = (Box6 $b) } }
}
$PlacardGeo = Write-Geo 'placard' $plBones
$comps = Base-Components 'placard' 1 1 '#c08d3e' @('pickaxe')
$comps['minecraft:light_dampening'] = 0
$comps['minecraft:geometry'] = [ordered]@{ identifier = $PlacardGeo; bone_visibility = $plVis }
$comps['minecraft:material_instances'] = [ordered]@{ '*' = (Mat (Tex 'placard' 'placard') 'alpha_test') }
$plPerms += [ordered]@{ condition = "$(Q powered) == 1"; components = [ordered]@{ 'minecraft:material_instances' = [ordered]@{ '*' = (Mat (Tex 'placard_powered' 'placard_powered') 'alpha_test') } } }
# Create: a powered placard gives 15 to every side and strongly powers the block it hangs on
foreach ($f in 0..5) {
    $back = $Dirs[@(1, 0, 3, 2, 5, 4)[$f]]
    $plPerms += [ordered]@{ condition = "$(Q powered) == 1 && $(Q facing) == $f"; components = [ordered]@{
            'minecraft:redstone_producer' = [ordered]@{ power = 15; strongly_powered_face = $back; connected_faces = $Dirs; transform_relative = $false }
        }
    }
}
$FormatVersion = '1.21.120' # minecraft:redstone_producer needs it
Block-Json 'placard' ([ordered]@{ 'levitate:facing' = @(0, 1, 2, 3, 4, 5); 'levitate:powered' = @(0, 1) }) $comps $plPerms
$FormatVersion = '1.21.90'
Loot 'placard'
$BlockSounds['levitate:placard'] = 'copper'

# ---- copycats -------------------------------------------------------------------------------------------------------------
$cpBones = @(); $cpVis = [ordered]@{}; $cpPerms = @()
foreach ($f in 0..5) {
    $b = Facing-Box @(0, 0, 0, 16, 3, 16) $f
    $cpBones += Bone "f$f" (Cube $b)
    $cpVis["f$f"] = "$(Q facing) == $f"
    $cpPerms += [ordered]@{ condition = "$(Q facing) == $f"; components = [ordered]@{ 'minecraft:collision_box' = (Box6 $b); 'minecraft:selection_box' = (Box6 $b) } }
}
$CopyPanelGeo = Write-Geo 'copycat_panel' $cpBones
$comps = Base-Components 'copycat_panel' 0.5 0.5 '#8a8d84' @('pickaxe', 'axe')
$comps['minecraft:light_dampening'] = 0
$comps['minecraft:geometry'] = [ordered]@{ identifier = $CopyPanelGeo; bone_visibility = $cpVis }
$comps['minecraft:material_instances'] = [ordered]@{ '*' = (Mat (Tex 'copycat_base' 'copycat_base') 'alpha_test') }
Block-Json 'copycat_panel' ([ordered]@{ 'levitate:facing' = @(0, 1, 2, 3, 4, 5) }) $comps $cpPerms
Loot 'copycat_panel'
$BlockSounds['levitate:copycat_panel'] = 'metal'

$csBones = @(); $csVis = [ordered]@{}; $csPerms = @()
foreach ($f in 2, 3, 4, 5) {
    foreach ($t in 0, 1) {
        $y0 = 8 * $t
        $b = Rot-Y @(0, $y0, 0, 16, ($y0 + 8), 8) $Quarter[$f]
        $csBones += Bone "f$f$t" (Cube $b)
        $csVis["f$f$t"] = "$(HF $f) && $(Q top) == $t"
        $csPerms += [ordered]@{ condition = "$(HF $f) && $(Q top) == $t"; components = [ordered]@{ 'minecraft:collision_box' = (Box6 $b); 'minecraft:selection_box' = (Box6 $b) } }
    }
}
$CopyStepGeo = Write-Geo 'copycat_step' $csBones
$comps = Base-Components 'copycat_step' 0.5 0.5 '#8a8d84' @('pickaxe', 'axe')
$comps['minecraft:light_dampening'] = 0
$comps['minecraft:geometry'] = [ordered]@{ identifier = $CopyStepGeo; bone_visibility = $csVis }
$comps['minecraft:material_instances'] = [ordered]@{ '*' = (Mat (Tex 'copycat_base' 'copycat_base') 'alpha_test') }
Block-Json 'copycat_step' ([ordered]@{ 'levitate:facing' = @(0, 1, 2, 3, 4, 5); 'levitate:top' = @(0, 1) }) $comps $csPerms
Loot 'copycat_step'
$BlockSounds['levitate:copycat_step'] = 'metal'

# ---- resource pack fragments ------------------------------------------------------------------------------------------
$blocksD = [ordered]@{}
foreach ($k in $BlockSounds.Keys) { $blocksD[$k] = [ordered]@{ sound = $BlockSounds[$k] } }
Write-Json 'assets/pack/rp/blocks.d/building.json' $blocksD
Write-Json 'assets/pack/rp/terrain_texture.d/building.json' $TerrainTextures

# ---- recipes ------------------------------------------------------------------------------------------------------------------
function Shaped([string]$name, [string[]]$pattern, $key, [string]$result, [int]$count, [string]$dir = 'assets/pack/bp/recipes') {
    $k = [ordered]@{}
    foreach ($e in $key.GetEnumerator()) { $k[$e.Key] = [ordered]@{ item = $e.Value } }
    $first = @($key.Values)[0]
    Write-Json "$dir/$name.json" ([ordered]@{ format_version = '1.20.10'; 'minecraft:recipe_shaped' = [ordered]@{
                description = [ordered]@{ identifier = "levitate:$name" }; tags = @('crafting_table'); pattern = $pattern; key = $k
                unlock = @([ordered]@{ item = $first }); result = [ordered]@{ item = $result; count = $count }
            }
        })
}
function Shapeless([string]$name, [string[]]$items, [string]$result, [int]$count, [string]$dir = 'assets/pack/bp/recipes', [string]$tag = 'crafting_table') {
    Write-Json "$dir/$name.json" ([ordered]@{ format_version = '1.20.10'; 'minecraft:recipe_shapeless' = [ordered]@{
                description = [ordered]@{ identifier = "levitate:$name" }; tags = @($tag)
                ingredients = @($items | ForEach-Object { [ordered]@{ item = $_ } })
                unlock = @([ordered]@{ item = $items[0] }); result = [ordered]@{ item = $result; count = $count }
            }
        })
}
function Stonecut([string]$name, [string]$from, [string]$result, [int]$count, [string]$dir = 'assets/pack/bp/recipes') {
    Shapeless $name @($from) $result $count $dir 'stonecutter'
}
function Smelt([string]$name, [string]$from, [string]$result) {
    Write-Json "assets/pack/bp/recipes/$name.json" ([ordered]@{ format_version = '1.20.10'; 'minecraft:recipe_furnace' = [ordered]@{
                description = [ordered]@{ identifier = "levitate:$name" }; tags = @('furnace'); input = $from; output = $result
            }
        })
}
$Pending = 'assets/pending_recipes/building'
$IronWindowBlock = @{ industrial_iron = 'levitate:industrial_iron_block'; weathered_iron = 'levitate:weathered_iron_block' }
foreach ($w in $WindowTypes) {
    $frame = if ($w -in $Woods) { "minecraft:${w}_planks" } elseif ($w -eq 'ornate_iron') { 'minecraft:iron_nugget' } else { $IronWindowBlock[$w] }
    $dir = if ($w -in @('industrial_iron', 'weathered_iron')) { $Pending } else { 'assets/pack/bp/recipes' }
    Shaped "${w}_window" @(' # ', '#X#') @{ '#' = $frame; X = 'minecraft:glass' } "levitate:${w}_window" 2 $dir
    Shaped "${w}_window_pane" @('###', '###') @{ '#' = "levitate:${w}_window" } "levitate:${w}_window_pane" 16
}
foreach ($g in $GlassKinds.Keys) {
    Stonecut "${g}_from_glass_stonecutting" 'minecraft:glass' "levitate:$g" 1
    Stonecut "${g}_pane_from_glass_pane_stonecutting" 'minecraft:glass_pane' "levitate:${g}_pane" 1
    Shaped "${g}_pane" @('###', '###') @{ '#' = "levitate:$g" } "levitate:${g}_pane" 16
    Smelt "glass_from_$g" "levitate:$g" 'minecraft:glass'
    Smelt "glass_pane_from_${g}_pane" "levitate:${g}_pane" 'minecraft:glass_pane'
}
foreach ($type in @('shingles', 'tiles')) {
    $single = $type.TrimEnd('s')
    Stonecut "copper_${type}_from_copper_ingot_stonecutting" 'minecraft:copper_ingot' "levitate:copper_$type" 2
    foreach ($waxed in @('', 'waxed_')) {
        foreach ($stage in $Stages) {
            $full = "levitate:${waxed}${stage}copper_$type"
            $stairs = "${waxed}${stage}copper_${single}_stairs"; $slab = "${waxed}${stage}copper_${single}_slab"
            Shaped $stairs @('X  ', 'XX ', 'XXX') @{ X = $full } "levitate:$stairs" 4
            Shaped $slab @('XXX') @{ X = $full } "levitate:$slab" 6
            Stonecut "${stairs}_stonecutting" $full "levitate:$stairs" 1
            Stonecut "${slab}_stonecutting" $full "levitate:$slab" 2
        }
    }
    foreach ($stage in $Stages) {
        foreach ($shape in @("copper_$type", "copper_${single}_stairs", "copper_${single}_slab")) {
            Shapeless "waxed_${stage}${shape}_from_honeycomb" @("levitate:${stage}$shape", 'minecraft:honeycomb') "levitate:waxed_${stage}$shape" 1
        }
    }
}
Stonecut 'copper_scaffolding_from_copper_ingot_stonecutting' 'minecraft:copper_ingot' 'levitate:copper_scaffolding' 2
Stonecut 'copper_ladder_from_copper_ingot_stonecutting' 'minecraft:copper_ingot' 'levitate:copper_ladder' 2
Stonecut 'copper_bars_from_copper_ingot_stonecutting' 'minecraft:copper_ingot' 'levitate:copper_bars' 4
Stonecut 'copper_table_cloth_from_copper_ingot_stonecutting' 'minecraft:copper_ingot' 'levitate:copper_table_cloth' 2
foreach ($pair in @(@('andesite', 'levitate:andesite_alloy'), @('brass', 'levitate:brass_ingot'))) {
    $m = $pair[0]; $src = $pair[1]
    Stonecut "${m}_scaffolding_from_${m}_stonecutting" $src "levitate:${m}_scaffolding" 2 $Pending
    Stonecut "${m}_ladder_from_${m}_stonecutting" $src "levitate:${m}_ladder" 2 $Pending
    Stonecut "${m}_bars_from_${m}_stonecutting" $src "levitate:${m}_bars" 4 $Pending
    Stonecut "${m}_table_cloth_from_${m}_stonecutting" $src "levitate:${m}_table_cloth" 2 $Pending
}
foreach ($c in $Colours) {
    Shapeless "${c}_table_cloth" @("minecraft:${c}_wool", 'levitate:andesite_alloy') "levitate:${c}_table_cloth" 2 $Pending
    foreach ($o in $Colours) {
        if ($o -eq $c) { continue }
        Shapeless "${c}_table_cloth_from_${o}_table_cloth" @("minecraft:${c}_dye", "levitate:${o}_table_cloth") "levitate:${c}_table_cloth" 1
    }
}
Stonecut 'copycat_panel_from_zinc_ingot_stonecutting' 'levitate:zinc_ingot' 'levitate:copycat_panel' 4 $Pending
Stonecut 'copycat_step_from_zinc_ingot_stonecutting' 'levitate:zinc_ingot' 'levitate:copycat_step' 4 $Pending
Shaped 'metal_girder' @('PPP', 'CCC') @{ P = 'levitate:iron_sheet'; C = 'levitate:andesite_alloy' } 'levitate:metal_girder' 8 $Pending
Shaped 'metal_bracket' @('SSS', 'PCP') @{ S = 'minecraft:iron_nugget'; P = 'minecraft:iron_ingot'; C = 'levitate:andesite_alloy' } 'levitate:metal_bracket' 4 $Pending
Shaped 'wooden_bracket' @('SSS', 'PCP') @{ S = 'minecraft:stick'; P = 'minecraft:oak_planks'; C = 'levitate:andesite_alloy' } 'levitate:wooden_bracket' 4 $Pending
Shapeless 'placard' @('minecraft:frame', 'levitate:brass_sheet') 'levitate:placard' 1 $Pending
foreach ($id in $Doors.Keys) {
    $i = 0
    foreach ($door in $WoodenDoors) {
        Shapeless "${id}_$i" @($door, "levitate:$($Doors[$id].casing)") "levitate:$id" 1
        $i++
    }
}
foreach ($id in $Trapdoors.Keys) {
    $i = 0
    foreach ($td in $WoodenTrapdoors) {
        Shapeless "${id}_$i" @($td, "levitate:$($Trapdoors[$id].casing)") "levitate:$id" 1
        $i++
    }
}

# processing recipes (Create's recipe types, read by U3's loader)
function Proc([string]$type, [string]$name, $obj) { Write-Json "assets/recipes/$type/$name.json" $obj }
foreach ($pair in @(@('andesite_casing', @{ item = 'levitate:andesite_alloy' }), @('brass_casing', @{ tag = 'c:ingots/brass' }), @('copper_casing', @{ tag = 'c:ingots/copper' }))) {
    foreach ($src in @('log', 'wood')) {
        Proc 'item_application' "$($pair[0])_from_$src" ([ordered]@{
                type        = 'levitate:item_application'
                ingredients = @([ordered]@{ tag = "c:stripped_${src}s" }, $pair[1])
                results     = @([ordered]@{ item = "levitate:$($pair[0])" })
            })
    }
}
Proc 'item_application' 'railway_casing' ([ordered]@{
        type = 'levitate:item_application'; ingredients = @([ordered]@{ item = 'levitate:brass_casing' }, [ordered]@{ tag = 'c:plates/obsidian' })
        results = @([ordered]@{ item = 'levitate:railway_casing' })
    })
foreach ($w in @('', '_pane')) {
    Proc 'splashing' "industrial_iron_window$w" ([ordered]@{
            type = 'levitate:splashing'; ingredients = @([ordered]@{ item = "levitate:industrial_iron_window$w" })
            results = @([ordered]@{ item = "levitate:weathered_iron_window$w" })
        })
}
foreach ($type in @('shingles', 'tiles')) {
    $single = $type.TrimEnd('s')
    foreach ($shape in @("copper_$type", "copper_${single}_stairs", "copper_${single}_slab")) {
        for ($si = 0; $si -lt 4; $si++) {
            $s = $Stages[$si]
            if ($si -gt 0) {
                Proc 'deploying' "$($Stages[$si-1])${shape}_from_deoxidising" ([ordered]@{
                        type = 'levitate:deploying'; ingredients = @([ordered]@{ item = "levitate:$s$shape" }, [ordered]@{ tag = 'minecraft:axes' })
                        results = @([ordered]@{ item = "levitate:$($Stages[$si-1])$shape" }); keep_held_item = $true
                    })
            }
            Proc 'deploying' "${s}${shape}_from_removing_wax" ([ordered]@{
                    type = 'levitate:deploying'; ingredients = @([ordered]@{ item = "levitate:waxed_$s$shape" }, [ordered]@{ tag = 'minecraft:axes' })
                    results = @([ordered]@{ item = "levitate:$s$shape" }); keep_held_item = $true
                })
            Proc 'deploying' "waxed_${s}${shape}_from_adding_wax" ([ordered]@{
                    type = 'levitate:deploying'; ingredients = @([ordered]@{ item = "levitate:$s$shape" }, [ordered]@{ item = 'minecraft:honeycomb_block' })
                    results = @([ordered]@{ item = "levitate:waxed_$s$shape" }); keep_held_item = $true
                })
        }
    }
}
Write-Json 'assets/recipes/tags/building.json' ([ordered]@{
        'c:stripped_logs'  = @('minecraft:stripped_oak_log', 'minecraft:stripped_spruce_log', 'minecraft:stripped_birch_log', 'minecraft:stripped_jungle_log',
            'minecraft:stripped_acacia_log', 'minecraft:stripped_dark_oak_log', 'minecraft:stripped_mangrove_log', 'minecraft:stripped_cherry_log',
            'minecraft:stripped_pale_oak_log', 'minecraft:stripped_crimson_stem', 'minecraft:stripped_warped_stem', 'minecraft:stripped_bamboo_block')
        'c:stripped_woods' = @('minecraft:stripped_oak_wood', 'minecraft:stripped_spruce_wood', 'minecraft:stripped_birch_wood', 'minecraft:stripped_jungle_wood',
            'minecraft:stripped_acacia_wood', 'minecraft:stripped_dark_oak_wood', 'minecraft:stripped_mangrove_wood', 'minecraft:stripped_cherry_wood',
            'minecraft:stripped_pale_oak_wood', 'minecraft:stripped_crimson_hyphae', 'minecraft:stripped_warped_hyphae')
        'minecraft:axes'   = @('minecraft:wooden_axe', 'minecraft:stone_axe', 'minecraft:copper_axe', 'minecraft:iron_axe', 'minecraft:golden_axe', 'minecraft:diamond_axe', 'minecraft:netherite_axe')
    })

# ---- list of generated files (lets the next run remove files that are no longer generated) --------
$listPath = Join-Path $PSScriptRoot 'building.generated.txt'
if (Test-Path $listPath) {
    foreach ($old in Get-Content $listPath) {
        if ($old -and -not $Generated.Contains($old)) { Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $root $old) }
    }
}
[System.IO.File]::WriteAllText($listPath, (($Generated | Sort-Object) -join "`n") + "`n", [System.Text.UTF8Encoding]::new($false))
Write-Host "building: $($Generated.Count) files, $($TerrainTextures.Count) terrain textures"
