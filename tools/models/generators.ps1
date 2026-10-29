# Models and pack data of the generators unit (U7): geometry (block models + the rotating parts the
# client mod draws), behavior pack blocks, loot tables, recipes and the resource pack fragments.
# Original models (not derived from Create's assets). Textures: tools/textures/generators.ps1.
#
#   pwsh tools/models/generators.ps1
#
# Geometry conventions (see src/core/kinetics/RotorModel.h): block models are built with the
# rotation axis / facing along +Y; a model x is world -x. Rotating parts for the kinetics renderer
# are modelled around +Y through the block centre.
#
# The large water wheel (48 px across) is larger than a Bedrock block model may be: the wheel block
# and its 8 structure blocks each show the part of the wheel inside their own cell (generated here
# per axis and cell); the client mod hides those and spins the whole wheel instead.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$rp = Join-Path $root 'assets/pack/rp'
$bp = Join-Path $root 'assets/pack/bp'
$pending = Join-Path $root 'assets/pending_recipes/generators'

$woods = @('oak', 'spruce', 'birch', 'jungle', 'acacia', 'dark_oak', 'mangrove', 'cherry', 'bamboo', 'crimson', 'warped', 'pale_oak')
$colours = @('white', 'orange', 'magenta', 'light_blue', 'yellow', 'lime', 'pink', 'gray', 'light_gray', 'cyan', 'purple', 'blue', 'brown', 'green', 'red', 'black')
$defaultWood = 1

function Write-Json($obj, [string]$path, [switch]$Compress) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
    $json = $obj | ConvertTo-Json -Depth 64 -Compress:$Compress
    [IO.File]::WriteAllText($path, $json.Replace("`r`n", "`n") + "`n")
}
function Rnd([double]$v) { [math]::Round($v, 4) }

# ---- parts: boxes in wheel coordinates (pixels): p, q in the plane, r along the axis ------------------------
# tex: main (0,0), second (16,0), third (0,16), shaft (16,16) regions of the 32x32 texture
$regions = @{ main = @(0, 0); second = @(16, 0); third = @(0, 16); shaft = @(16, 16) }
function Part($p0, $p1, $q0, $q1, $r0, $r1, $tex, $rot = 0) {
    [pscustomobject]@{ p0 = [double]$p0; p1 = [double]$p1; q0 = [double]$q0; q1 = [double]$q1; r0 = [double]$r0; r1 = [double]$r1; tex = $tex; rot = [double]$rot }
}
# quarter turns in the plane: (p, q) -> (-q, p)
function Turn($part, [int]$quarters) {
    $x = $part
    for ($i = 0; $i -lt (($quarters % 4) + 4) % 4; $i++) {
        $x = Part (-$x.q1) (-$x.q0) $x.p0 $x.p1 $x.r0 $x.r1 $x.tex $x.rot
    }
    $x
}
# the same part turned by `deg` about the wheel centre (a multiple of 22.5)
function Spin($part, [double]$deg) {
    $quarters = [int][math]::Round($deg / 90)
    $x = Turn $part $quarters
    $x.rot = $deg - 90 * $quarters
    $x
}
function Flip($part) { Part $part.p0 $part.p1 (-$part.q1) (-$part.q0) (-$part.r1) (-$part.r0) $part.tex $part.rot }

# A Bedrock cube for `part` in the cell (a, b) of a wheel along `axis` ('x', 'y', 'z'); $null when
# the part is outside the cell. $clip: cut axis-aligned parts at the cell's edges.
function Cube($part, [string]$axis, [int]$a = 0, [int]$b = 0, [bool]$clip = $false, [int]$voff = 0) {
    $p0 = $part.p0 - 16 * $a; $p1 = $part.p1 - 16 * $a; $q0 = $part.q0 - 16 * $b; $q1 = $part.q1 - 16 * $b
    if ($clip) {
        if ($part.rot -eq 0) {
            $p0 = [math]::Max($p0, -8); $p1 = [math]::Min($p1, 8); $q0 = [math]::Max($q0, -8); $q1 = [math]::Min($q1, 8)
            if ($p1 - $p0 -lt 0.01 -or $q1 - $q0 -lt 0.01) { return $null }
        } else {
            $pc = ($part.p0 + $part.p1) / 2; $qc = ($part.q0 + $part.q1) / 2
            $t = $part.rot * [math]::PI / 180
            $rp_ = $pc * [math]::Cos($t) - $qc * [math]::Sin($t); $rq = $pc * [math]::Sin($t) + $qc * [math]::Cos($t)
            if ([math]::Floor(($rp_ + 8) / 16) -ne $a -or [math]::Floor(($rq + 8) / 16) -ne $b) { return $null }
        }
    }
    $r0 = $part.r0; $r1 = $part.r1
    $pp = -16 * $a; $pq = -16 * $b # the wheel centre in cell coordinates
    switch ($axis) {
        'y' { $X = @($p0, $p1); $Y = @(($r0 + 8), ($r1 + 8)); $Z = @($q0, $q1); $piv = @($pp, 8, $pq); $rot = @(0, $part.rot, 0) }
        'x' { $X = @($r0, $r1); $Y = @(($p0 + 8), ($p1 + 8)); $Z = @($q0, $q1); $piv = @(0, (8 + $pp), $pq); $rot = @($part.rot, 0, 0) }
        'z' { $X = @($p0, $p1); $Y = @(($q0 + 8), ($q1 + 8)); $Z = @($r0, $r1); $piv = @($pp, (8 + $pq), 0); $rot = @(0, 0, (-$part.rot)) }
    }
    # world rotation about +axis by theta: y: -rot, x: rot, z: rot; model x is mirrored, which
    # negates rotations about y and z (the values above already include that)
    $sx = $X[1] - $X[0]; $sy = $Y[1] - $Y[0]; $sz = $Z[1] - $Z[0]
    $reg = $regions[$part.tex]
    $face = {
        param($w, $h)
        [ordered]@{ uv = @($reg[0], ($reg[1] + $voff)); uv_size = @((Rnd ([math]::Min(16, $w))), (Rnd ([math]::Min(16, $h)))) }
    }
    $cube = [ordered]@{
        origin = @((Rnd (-$X[1])), (Rnd $Y[0]), (Rnd $Z[0]))
        size   = @((Rnd $sx), (Rnd $sy), (Rnd $sz))
    }
    if ($part.rot -ne 0) {
        $cube.pivot = @((Rnd (-$piv[0])), (Rnd $piv[1]), (Rnd $piv[2]))
        $cube.rotation = @((Rnd $rot[0]), (Rnd $rot[1]), (Rnd $rot[2]))
    }
    $cube.uv = [ordered]@{
        north = & $face $sx $sy; south = & $face $sx $sy
        east  = & $face $sz $sy; west = & $face $sz $sy
        up    = & $face $sx $sz; down = & $face $sx $sz
    }
    $cube
}
function Bone([string]$name, $cubes) { [ordered]@{ name = $name; pivot = @(0, 8, 0); cubes = @($cubes | Where-Object { $_ }) } }
function Geometry([string]$id, $bones, [int]$th = 32) {
    [ordered]@{
        description = [ordered]@{ identifier = $id; texture_width = 32; texture_height = $th; visible_bounds_width = 4; visible_bounds_height = 4; visible_bounds_offset = @(0, 0.5, 0) }
        bones       = @($bones)
    }
}
function GeoFile($geometries) { [ordered]@{ format_version = '1.16.0'; 'minecraft:geometry' = @($geometries) } }
function Cubes($parts, $axis = 'y', $voff = 0) { @($parts | ForEach-Object { Cube $_ $axis 0 0 $false $voff }) }

# ---- hand crank (facing up: the shaft goes down into the block below) ------------------------------------------
$crankShaft = @(Part -2 2 -2 2 -8 -2 shaft)
$crankHandle = @(
    (Part -3 3 -3 3 -2 1 third),    # hub
    (Part -1 7 -1 1 0 2 main),      # arm
    (Part 5 7 -1 1 2 7 second)      # grip
)
# ---- valve handle (facing up: a wheel close to the mounting face) -----------------------------------------------
$valveShaft = @(Part -2 2 -2 2 -8 -6 shaft)
$valveHandle = @(
    (Part -2.5 2.5 -2.5 2.5 -6 -3.5 third),
    (Part -7 7 5 7 -6 -4 main), (Part -7 7 -7 -5 -6 -4 main), (Part 5 7 -5 5 -6 -4 main), (Part -7 -5 -5 5 -6 -4 main),
    (Part -5 5 -0.75 0.75 -5.5 -4.5 second), (Part -0.75 0.75 -5 5 -5.5 -4.5 second)
)
# ---- creative motor (facing up: the shaft points up) ------------------------------------------------------------
$motorBody = @(
    (Part -7 7 -7 7 -8 -6 second),
    (Part -6 6 -6 6 -6 3 main),
    (Part -6.5 6.5 -6.5 6.5 -4 -3 third), (Part -6.5 6.5 -6.5 6.5 -1 0 third),
    (Part -4 4 -4 4 3 4 second)
)
$motorShaft = @(Part -2 2 -2 2 4 8 shaft)

# ---- water wheel (radius 8, 8 paddles) ----------------------------------------------------------------------------
$small = @(
    (Part -2 2 -2 2 -8 8 shaft),
    (Part -3 3 -3 3 -5 5 third),
    (Part -6 6 -1 1 -1 1 second), (Part -1 1 -6 6 -1 1 second)
)
$rim = Part 5 6.5 -2.5 2.5 -6 6 main
$paddle = Part 3 8 -0.75 0.75 -7.5 7.5 main
foreach ($k in 0..3) { $small += Spin $rim (90 * $k); $small += Spin $rim (45 + 90 * $k) }
foreach ($k in 0..7) { $small += Spin $paddle (45 * $k) }

# ---- large water wheel (radius 24, 16 paddles) --------------------------------------------------------------------
$large = @(
    (Part -2 2 -2 2 -8 8 shaft),
    (Part -4 4 -4 4 -6 6 third)
)
$spoke = Part 4 19 -1 1 -2 2 second
foreach ($k in 0..3) { $large += Turn $spoke $k }
$diagIn = Part 4 11 -1 1 -2 2 second
$diagOut = Part 11 20 -1 1 -2 2 second
foreach ($k in 0..3) { $large += Spin $diagIn (45 + 90 * $k); $large += Spin $diagOut (45 + 90 * $k) }
$rimL = Part 20 22 -8.3 8.3 -6 6 main
$rimD = Part 20 22 -8.7 8.7 -6 6 main
foreach ($k in 0..3) { $large += Turn $rimL $k; $large += Spin $rimD (45 + 90 * $k) }
$paddleL = Part 14 24 -1 1 -7 7 main
foreach ($k in 0..15) { $large += Spin $paddleL (22.5 * $k) }

# ---- geometry files ------------------------------------------------------------------------------------------------
$models = Join-Path $rp 'models/blocks'
function HandleRotor($id, $shaft, $handle) {
    Geometry $id @(
        (Bone 'shaft_pos' (Cubes $shaft)), (Bone 'shaft_neg' (Cubes ($shaft | ForEach-Object { Flip $_ }))),
        (Bone 'handle_pos' (Cubes $handle)), (Bone 'handle_neg' (Cubes ($handle | ForEach-Object { Flip $_ })))
    )
}
Write-Json (GeoFile @(
        (Geometry 'geometry.levitate.hand_crank' @((Bone 'shaft' (Cubes $crankShaft)), (Bone 'handle' (Cubes $crankHandle)))),
        (HandleRotor 'geometry.levitate.hand_crank.rotor' $crankShaft $crankHandle)
    )) "$models/hand_crank.geo.json" -Compress
Write-Json (GeoFile @(
        (Geometry 'geometry.levitate.valve_handle' @((Bone 'shaft' (Cubes $valveShaft)), (Bone 'handle' (Cubes $valveHandle)))),
        (HandleRotor 'geometry.levitate.valve_handle.rotor' $valveShaft $valveHandle)
    )) "$models/valve_handle.geo.json" -Compress
Write-Json (GeoFile @(
        (Geometry 'geometry.levitate.creative_motor' @((Bone 'body' (Cubes $motorBody)), (Bone 'shaft' (Cubes $motorShaft)))),
        (Geometry 'geometry.levitate.creative_motor.rotor' @((Bone 'shaft_pos' (Cubes $motorShaft)), (Bone 'shaft_neg' (Cubes ($motorShaft | ForEach-Object { Flip $_ })))))
    )) "$models/creative_motor.geo.json" -Compress
$sheetHeight = 32 * $woods.Count
Write-Json (GeoFile @(
        (Geometry 'geometry.levitate.water_wheel' @((Bone 'wheel' (Cubes $small)))),
        (Geometry 'geometry.levitate.water_wheel.rotor' @(0..($woods.Count - 1) | ForEach-Object { Bone "wheel_$_" (Cubes $small 'y' (32 * $_)) }) $sheetHeight)
    )) "$models/water_wheel.geo.json" -Compress
$largeGeos = @()
foreach ($axis in 'x', 'y', 'z') {
    foreach ($a in -1..1) {
        foreach ($b in -1..1) {
            $cubes = @($large | ForEach-Object { Cube $_ $axis $a $b $true 0 })
            $largeGeos += Geometry "geometry.levitate.large_water_wheel.$axis$($a + 1)$($b + 1)" @((Bone 'cell' $cubes))
        }
    }
}
$largeGeos += Geometry 'geometry.levitate.large_water_wheel.rotor' @(0..($woods.Count - 1) | ForEach-Object { Bone "wheel_$_" (Cubes $large 'y' (32 * $_)) }) $sheetHeight
Write-Json (GeoFile $largeGeos) "$models/large_water_wheel.geo.json" -Compress

# ---- behavior pack blocks ---------------------------------------------------------------------------------------------
function Range($n) { @(0..($n - 1)) }
function Box($o, $s) { [ordered]@{ origin = $o; size = $s } }
# Y-up models turned to a facing (0 down, 1 up, 2 north, 3 south, 4 west, 5 east)
$facingRotation = @{ 0 = @(180, 0, 0); 1 = @(0, 0, 0); 2 = @(-90, 0, 0); 3 = @(90, 0, 0); 4 = @(0, 0, 90); 5 = @(0, 0, -90) }
function FacingPermutations() {
    foreach ($f in 0..5) {
        [ordered]@{ condition = "q.block_state('levitate:facing') == $f"; components = [ordered]@{ 'minecraft:transformation' = [ordered]@{ rotation = $facingRotation[$f] } } }
    }
}
function Material($texture) { [ordered]@{ '*' = [ordered]@{ texture = $texture; render_method = 'alpha_test' } } }
function BlockFile($id, $category, $states, $components, $permutations) {
    $desc = [ordered]@{ identifier = "levitate:$id"; menu_category = [ordered]@{ category = $category } }
    $desc.states = $states
    $block = [ordered]@{ description = $desc; components = $components }
    if ($permutations) { $block.permutations = @($permutations) }
    Write-Json ([ordered]@{ format_version = '1.21.40'; 'minecraft:block' = $block }) "$bp/blocks/$id.json"
}
function Loot($id, $drop) {
    $pools = if ($drop) { @([ordered]@{ rolls = 1; entries = @([ordered]@{ type = 'item'; name = $drop; weight = 1 }) }) } else { @() }
    Write-Json ([ordered]@{ pools = $pools }) "$bp/loot_tables/blocks/$id.json"
}
function Common($id, $seconds, $resistance, $collision, $selection, $mapColor) {
    [ordered]@{
        'minecraft:collision_box'              = $collision
        'minecraft:selection_box'              = $selection
        'minecraft:destructible_by_mining'     = [ordered]@{ seconds_to_destroy = $seconds }
        'minecraft:destructible_by_explosion'  = [ordered]@{ explosion_resistance = $resistance }
        'minecraft:light_dampening'            = 0
        'minecraft:map_color'                  = $mapColor
        'minecraft:loot'                       = "loot_tables/blocks/$id.json"
    }
}
function HandleBlock($id, $geometry, $texture, $collision, $selection, $mapColor, $seconds) {
    $c = [ordered]@{
        'minecraft:geometry'           = [ordered]@{ identifier = $geometry; bone_visibility = [ordered]@{ shaft = "q.block_state('levitate:rotor') == 0"; handle = "q.block_state('levitate:rotor') == 0" } }
        'minecraft:material_instances' = Material $texture
    }
    foreach ($e in (Common $id $seconds 3 $collision $selection $mapColor).GetEnumerator()) { $c[$e.Key] = $e.Value }
    BlockFile $id 'construction' ([ordered]@{ 'levitate:facing' = Range 6; 'levitate:rotor' = Range 2 }) $c (FacingPermutations)
    Loot $id "levitate:$id"
}

HandleBlock 'hand_crank' 'geometry.levitate.hand_crank' 'levitate_hand_crank' (Box @(-5, 0, -5) @(10, 9, 10)) (Box @(-5, 0, -5) @(10, 12, 10)) '#8f6f45' 1.0
HandleBlock 'copper_valve_handle' 'geometry.levitate.valve_handle' 'levitate_copper_valve_handle' (Box @(-7, 0, -7) @(14, 4, 14)) (Box @(-7, 0, -7) @(14, 4, 14)) '#c46f4a' 1.5
foreach ($colour in $colours) {
    HandleBlock "$($colour)_valve_handle" 'geometry.levitate.valve_handle' "levitate_$($colour)_valve_handle" (Box @(-7, 0, -7) @(14, 4, 14)) (Box @(-7, 0, -7) @(14, 4, 14)) '#c46f4a' 1.5
}

# creative motor
$c = [ordered]@{
    'minecraft:geometry'           = [ordered]@{ identifier = 'geometry.levitate.creative_motor'; bone_visibility = [ordered]@{ body = $true; shaft = "q.block_state('levitate:rotor') == 0" } }
    'minecraft:material_instances' = Material 'levitate_creative_motor'
}
foreach ($e in (Common 'creative_motor' 1.5 6 (Box @(-8, 0, -8) @(16, 16, 16)) (Box @(-8, 0, -8) @(16, 16, 16)) '#8a5fb8').GetEnumerator()) { $c[$e.Key] = $e.Value }
BlockFile 'creative_motor' 'construction' ([ordered]@{ 'levitate:facing' = Range 6; 'levitate:rotor' = Range 2 }) $c (FacingPermutations)
Loot 'creative_motor' 'levitate:creative_motor'

function WoodPermutations() {
    foreach ($w in 0..($woods.Count - 1)) {
        [ordered]@{ condition = "q.block_state('levitate:wood') == $w"; components = [ordered]@{ 'minecraft:material_instances' = Material "levitate_water_wheel_$($woods[$w])" } }
    }
}
$full = Box @(-8, 0, -8) @(16, 16, 16)

# water wheel: the Y model turned onto the facing's axis
$c = [ordered]@{
    'minecraft:geometry'           = [ordered]@{ identifier = 'geometry.levitate.water_wheel'; bone_visibility = [ordered]@{ wheel = "q.block_state('levitate:rotor') == 0" } }
    'minecraft:material_instances' = Material "levitate_water_wheel_$($woods[$defaultWood])"
}
foreach ($e in (Common 'water_wheel' 2.0 3 $full $full '#6e4f2f').GetEnumerator()) { $c[$e.Key] = $e.Value }
$perms = @(WoodPermutations)
$perms += [ordered]@{ condition = "q.block_state('levitate:facing') >= 4"; components = [ordered]@{ 'minecraft:transformation' = [ordered]@{ rotation = @(0, 0, 90) } } }
$perms += [ordered]@{ condition = "q.block_state('levitate:facing') == 2 || q.block_state('levitate:facing') == 3"; components = [ordered]@{ 'minecraft:transformation' = [ordered]@{ rotation = @(90, 0, 0) } } }
BlockFile 'water_wheel' 'construction' ([ordered]@{ 'levitate:facing' = Range 6; 'levitate:wood' = Range $woods.Count; 'levitate:rotor' = Range 2 }) $c $perms
Loot 'water_wheel' 'levitate:water_wheel'

# large water wheel: the centre cell of its axis
$axisNames = @('x', 'y', 'z')
$c = [ordered]@{
    'minecraft:geometry'           = [ordered]@{ identifier = 'geometry.levitate.large_water_wheel.y11'; bone_visibility = [ordered]@{ cell = "q.block_state('levitate:rotor') == 0" } }
    'minecraft:material_instances' = Material "levitate_water_wheel_$($woods[$defaultWood])"
}
foreach ($e in (Common 'large_water_wheel' 2.0 3 $full $full '#6e4f2f').GetEnumerator()) { $c[$e.Key] = $e.Value }
$perms = @(WoodPermutations)
foreach ($ax in 0..2) {
    $perms += [ordered]@{ condition = "q.block_state('levitate:axis') == $ax"; components = [ordered]@{ 'minecraft:geometry' = [ordered]@{ identifier = "geometry.levitate.large_water_wheel.$($axisNames[$ax])11"; bone_visibility = [ordered]@{ cell = "q.block_state('levitate:rotor') == 0" } } } }
}
BlockFile 'large_water_wheel' 'construction' ([ordered]@{ 'levitate:axis' = Range 3; 'levitate:extension' = Range 2; 'levitate:wood' = Range $woods.Count; 'levitate:rotor' = Range 2 }) $c $perms
Loot 'large_water_wheel' 'levitate:large_water_wheel'

# structure blocks: one cell of the wheel each (segment -> cell as core::generators::segmentCell)
$segmentCells = @(@(-1, -1), @(0, -1), @(1, -1), @(1, 0), @(1, 1), @(0, 1), @(-1, 1), @(-1, 0))
$c = [ordered]@{
    'minecraft:geometry'           = [ordered]@{ identifier = 'geometry.levitate.large_water_wheel.y00'; bone_visibility = [ordered]@{ cell = "q.block_state('levitate:rotor') == 0" } }
    'minecraft:material_instances' = Material "levitate_water_wheel_$($woods[$defaultWood])"
}
foreach ($e in (Common 'water_wheel_structure' 2.0 3 $full $full '#6e4f2f').GetEnumerator()) { $c[$e.Key] = $e.Value }
$perms = @(WoodPermutations)
foreach ($ax in 0..2) {
    foreach ($s in 0..7) {
        $cell = $segmentCells[$s]
        $perms += [ordered]@{
            condition  = "q.block_state('levitate:axis') == $ax && q.block_state('levitate:segment') == $s"
            components = [ordered]@{ 'minecraft:geometry' = [ordered]@{ identifier = "geometry.levitate.large_water_wheel.$($axisNames[$ax])$($cell[0] + 1)$($cell[1] + 1)"; bone_visibility = [ordered]@{ cell = "q.block_state('levitate:rotor') == 0" } } }
        }
    }
}
BlockFile 'water_wheel_structure' 'none' ([ordered]@{ 'levitate:segment' = Range 8; 'levitate:axis' = Range 3; 'levitate:wood' = Range $woods.Count; 'levitate:rotor' = Range 2 }) $c $perms
Loot 'water_wheel_structure' $null

# ---- recipes ------------------------------------------------------------------------------------------------------------
function Shaped($dir, $id, $pattern, $key, $unlock) {
    Write-Json ([ordered]@{ format_version = '1.20.10'; 'minecraft:recipe_shaped' = [ordered]@{
                description = [ordered]@{ identifier = "levitate:$id" }; tags = @('crafting_table'); pattern = $pattern; key = $key
                unlock = @([ordered]@{ item = $unlock }); result = [ordered]@{ item = "levitate:$id" }
            }
        }) "$dir/$id.json"
}
Shaped "$bp/recipes" 'large_water_wheel' @('PPP', 'PWP', 'PPP') ([ordered]@{ P = [ordered]@{ tag = 'minecraft:planks' }; W = [ordered]@{ item = 'levitate:water_wheel' } }) 'levitate:water_wheel'
foreach ($colour in $colours) {
    Write-Json ([ordered]@{ format_version = '1.20.10'; 'minecraft:recipe_shapeless' = [ordered]@{
                description = [ordered]@{ identifier = "levitate:$($colour)_valve_handle" }; tags = @('crafting_table')
                ingredients = @([ordered]@{ item = 'levitate:copper_valve_handle' }, [ordered]@{ item = "minecraft:$($colour)_dye" })
                unlock = @([ordered]@{ item = 'levitate:copper_valve_handle' }); result = [ordered]@{ item = "levitate:$($colour)_valve_handle"; count = 1 }
            }
        }) "$bp/recipes/$($colour)_valve_handle.json"
}
# ingredients of other units (U9 andesite alloy / copper sheet, U6 shaft): moved into the BP when those land
Shaped $pending 'hand_crank' @('PPP', '  A') ([ordered]@{ P = [ordered]@{ tag = 'minecraft:planks' }; A = [ordered]@{ item = 'levitate:andesite_alloy' } }) 'levitate:andesite_alloy'
Shaped $pending 'copper_valve_handle' @('CCC', ' A ') ([ordered]@{ C = [ordered]@{ item = 'levitate:copper_sheet' }; A = [ordered]@{ item = 'levitate:andesite_alloy' } }) 'levitate:copper_sheet'
Shaped $pending 'water_wheel' @('SSS', 'SAS', 'SSS') ([ordered]@{ S = [ordered]@{ tag = 'minecraft:wooden_slabs' }; A = [ordered]@{ item = 'levitate:shaft' } }) 'levitate:shaft'

# ---- resource pack fragments ------------------------------------------------------------------------------------------------
$terrain = [ordered]@{ levitate_hand_crank = [ordered]@{ textures = 'textures/blocks/hand_crank' } }
$terrain['levitate_copper_valve_handle'] = [ordered]@{ textures = 'textures/blocks/copper_valve_handle' }
foreach ($colour in $colours) { $terrain["levitate_$($colour)_valve_handle"] = [ordered]@{ textures = "textures/blocks/$($colour)_valve_handle" } }
foreach ($wood in $woods) { $terrain["levitate_water_wheel_$wood"] = [ordered]@{ textures = "textures/blocks/water_wheel_$wood" } }
$terrain['levitate_water_wheel_rotor'] = [ordered]@{ textures = 'textures/blocks/water_wheel_rotor' }
$terrain['levitate_creative_motor'] = [ordered]@{ textures = 'textures/blocks/creative_motor' }
Write-Json $terrain "$rp/terrain_texture.d/generators.json"

$sounds = [ordered]@{ 'levitate:hand_crank' = [ordered]@{ sound = 'wood' }; 'levitate:copper_valve_handle' = [ordered]@{ sound = 'copper' } }
foreach ($colour in $colours) { $sounds["levitate:$($colour)_valve_handle"] = [ordered]@{ sound = 'copper' } }
$sounds['levitate:water_wheel'] = [ordered]@{ sound = 'wood' }
$sounds['levitate:large_water_wheel'] = [ordered]@{ sound = 'wood' }
$sounds['levitate:water_wheel_structure'] = [ordered]@{ sound = 'wood' }
$sounds['levitate:creative_motor'] = [ordered]@{ sound = 'metal' }
Write-Json $sounds "$rp/blocks.d/generators.json"

$en = [ordered]@{ hand_crank = 'Hand Crank'; copper_valve_handle = 'Copper Valve Handle' }
$zh = [ordered]@{ hand_crank = '手摇曲柄'; copper_valve_handle = '铜阀门手轮' }
$enColour = @{ white = 'White'; orange = 'Orange'; magenta = 'Magenta'; light_blue = 'Light Blue'; yellow = 'Yellow'; lime = 'Lime'; pink = 'Pink'; gray = 'Gray'; light_gray = 'Light Gray'; cyan = 'Cyan'; purple = 'Purple'; blue = 'Blue'; brown = 'Brown'; green = 'Green'; red = 'Red'; black = 'Black' }
$zhColour = @{ white = '白色'; orange = '橙色'; magenta = '品红色'; light_blue = '淡蓝色'; yellow = '黄色'; lime = '黄绿色'; pink = '粉红色'; gray = '灰色'; light_gray = '淡灰色'; cyan = '青色'; purple = '紫色'; blue = '蓝色'; brown = '棕色'; green = '绿色'; red = '红色'; black = '黑色' }
foreach ($colour in $colours) { $en["$($colour)_valve_handle"] = "$($enColour[$colour]) Valve Handle"; $zh["$($colour)_valve_handle"] = "$($zhColour[$colour])阀门手轮" }
$en.water_wheel = 'Water Wheel'; $zh.water_wheel = '水车'
$en.large_water_wheel = 'Large Water Wheel'; $zh.large_water_wheel = '大型水车'
$en.water_wheel_structure = 'Large Water Wheel'; $zh.water_wheel_structure = '大型水车'
$en.creative_motor = 'Creative Motor'; $zh.creative_motor = '创造马达'
foreach ($pair in @(@('en_US', $en), @('zh_CN', $zh))) {
    $lines = foreach ($e in $pair[1].GetEnumerator()) { "tile.levitate:$($e.Key).name=$($e.Value)" }
    $path = "$rp/texts.d/$($pair[0])/generators.lang"
    New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
    [IO.File]::WriteAllText($path, (($lines -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
}
Write-Host 'generators models and pack data written'
