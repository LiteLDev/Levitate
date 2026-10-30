# Models and behavior pack blocks of the U49 additions to the transmission unit (U6): the cuckoo
# clock (and the mysterious cuckoo clock, the same model) and the flywheel. The other transmission
# blocks are written by hand. Original models (not derived from Create's assets). Textures:
# tools/textures/transmission.ps1 (cuckoo_clock.png, flywheel.png).
#
#   pwsh tools/models/transmission.ps1
#
# Cuckoo clock: built facing north (dial towards -z) in block pixels (0..16, a model x is world
# -x); other facings turn it about +Y (`minecraft:transformation`, the gauge's convention). Bones
# the client mod draws itself (client/transmission/CuckooClockRenderer.cpp) are hidden with
# `levitate:rotor` = 1: the hands (`hour_0` / `minute_0` point at 12 and are turned about the dial
# centre (8, 6, 2.5)), the doors (hinges at x 10 and 6) and the figures (drawn at their "out"
# place, slid back by half a block while the doors close). Vanilla clients see the bones the
# server's states pick: `levitate:hour` / `levitate:minute` (12 hand positions each, drawn as
# pixel steps since block models only turn cubes by 22.5 degrees) and `levitate:cuckoo`.
#
# Flywheel: a brass wheel 28 px across around +Y (a 16-gon rim of 22.5-degree segments), turned
# onto `levitate:axis`; the kinetics renderer spins bones `wheel` and `shaft` of the same geometry.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$rp = Join-Path $root 'assets/pack/rp'
$bp = Join-Path $root 'assets/pack/bp'
$models = Join-Path $rp 'models/blocks'

function Write-Json($obj, [string]$path, [switch]$Compress) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
    $json = $obj | ConvertTo-Json -Depth 64 -Compress:$Compress
    [IO.File]::WriteAllText($path, $json.Replace("`r`n", "`n") + "`n")
}
function Rnd([double]$v) { [math]::Round($v, 4) }
function Range($n) { @(0..($n - 1)) }
function GeoFile($geometries) { [ordered]@{ format_version = '1.16.0'; 'minecraft:geometry' = @($geometries) } }
function Geometry([string]$id, $bones, [int]$tw = 32, [int]$th = 32) {
    [ordered]@{
        description = [ordered]@{ identifier = $id; texture_width = $tw; texture_height = $th; visible_bounds_width = 4; visible_bounds_height = 4; visible_bounds_offset = @(0, 0.5, 0) }
        bones       = @($bones)
    }
}
function Bone([string]$name, $cubes) { [ordered]@{ name = $name; pivot = @(0, 8, 0); cubes = @($cubes | Where-Object { $_ }) } }

# ---- cuckoo clock --------------------------------------------------------------------------------------------
# texture regions of cuckoo_clock.png (32x32): body planks (0,0), roof shingles (16,0), dial 8x8 at
# (0,16), swatches 4x4 at (16,16): H hands, B brass, D door, O opening; (16,20): P pig, Q snout,
# G creeper, F creeper face. The static shaft uses the transmission rotor texture (material "shaft").
$swatch = @{ H = @(16, 16); B = @(20, 16); D = @(24, 16); O = @(28, 16); P = @(16, 20); Q = @(20, 20); G = @(24, 20); F = @(28, 20) }

# a face's uv: positional for the body / roof (so planks line up), whole regions otherwise
function FaceUv([string]$tex, [string]$face, $b, [double]$w, [double]$h) {
    switch ($tex) {
        { $_ -in 'body', 'roof' } {
            $base = if ($tex -eq 'body') { 0 } else { 16 }
            switch ($face) {
                { $_ -in 'north', 'south' } { $u = 16 - $b.x1; $v = 16 - $b.y1 }
                { $_ -in 'east', 'west' } { $u = $b.z0; $v = 16 - $b.y1 }
                default { $u = 16 - $b.x1; $v = $b.z0 }
            }
            $u = [math]::Min([math]::Max($u, 0), 16 - [math]::Min($w, 16)); $v = [math]::Min([math]::Max($v, 0), 16 - [math]::Min($h, 16))
            return [ordered]@{ uv = @((Rnd ($base + $u)), (Rnd $v)); uv_size = @((Rnd ([math]::Min($w, 16))), (Rnd ([math]::Min($h, 16)))) }
        }
        'dial' { return [ordered]@{ uv = @(0, 16); uv_size = @(8, 8) } }
        'shaft' {
            # transmission_rotor.png is 64x64: shaft side (0,0) 4x16, shaft end (4,0) 4x4, in 32-px units
            if ($face -in 'north', 'south') { return [ordered]@{ uv = @(2, 0); uv_size = @(2, 2); material_instance = 'shaft' } }
            return [ordered]@{ uv = @(0, 0); uv_size = @(2, 1); material_instance = 'shaft' }
        }
        default {
            $s = $swatch[$tex]
            if ($tex -eq 'D') { return [ordered]@{ uv = @($s[0], $s[1]); uv_size = @((Rnd ([math]::Min($w, 4))), (Rnd ([math]::Min($h, 4)))) } }
            return [ordered]@{ uv = @(($s[0] + 1), ($s[1] + 1)); uv_size = @(1, 1) }
        }
    }
}
# a box in block pixels (north-facing frame) -> a Bedrock cube; $tex: one texture or a hashtable per
# local face name (north = the clock's front), key '*' for the rest
function LBox([double]$x0, [double]$y0, [double]$z0, [double]$x1, [double]$y1, [double]$z1, $tex) {
    $b = [pscustomobject]@{ x0 = $x0; y0 = $y0; z0 = $z0; x1 = $x1; y1 = $y1; z1 = $z1 }
    $sx = $x1 - $x0; $sy = $y1 - $y0; $sz = $z1 - $z0
    $cube = [ordered]@{ origin = @((Rnd (8 - $x1)), (Rnd $y0), (Rnd ($z0 - 8))); size = @((Rnd $sx), (Rnd $sy), (Rnd $sz)) }
    $uv = [ordered]@{}
    # local face -> model face (a model x is world -x)
    foreach ($f in @(@('north', 'north', $sx, $sy), @('south', 'south', $sx, $sy), @('east', 'west', $sz, $sy), @('west', 'east', $sz, $sy), @('up', 'up', $sx, $sz), @('down', 'down', $sx, $sz))) {
        $t = if ($tex -is [hashtable]) { if ($tex.ContainsKey($f[0])) { $tex[$f[0]] } else { $tex['*'] } } else { $tex }
        $uv[$f[1]] = FaceUv $t $f[0] $b $f[2] $f[3]
    }
    $cube.uv = $uv
    $cube
}

$clockBody = @(
    (LBox 1 0 2 15 1 15 'roof'),                          # plinth
    (LBox 2 1 3 14 10 14 'body'),
    (LBox 2 10 3 6 13 14 'body'), (LBox 10 10 3 14 13 14 'body'),
    (LBox 6 10 5 10 13 14 @{ north = 'O'; '*' = 'body' }), # the door opening's back wall
    (LBox 4 2 2.5 12 10 3 @{ north = 'dial'; '*' = 'body' }),
    (LBox 7.5 5.5 1.9 8.5 6.5 2.5 'B')                   # the hands' pin
)
$roof = @()
for ($i = 0; $i -lt 6; $i++) { $roof += LBox (1 + $i) (13 + $i) 2 (15 - $i) (14 + $i) 15 'roof' }

# hands: pixel steps from the dial centre (8, 6), clockwise as seen from the front (the viewer's
# right is -x); hour: 2 steps, minute: 3
function HandCubes([double]$deg, [int]$len, [double]$z0, [double]$z1) {
    $t = $deg * [math]::PI / 180
    $cells = @{}
    for ($d = 1; $d -le $len; $d++) {
        $cx = [math]::Round((8 - $d * [math]::Sin($t)) * 2) / 2
        $cy = [math]::Round((6 + $d * [math]::Cos($t)) * 2) / 2
        $cells["$cx,$cy"] = @($cx, $cy)
    }
    foreach ($c in $cells.Values) { LBox ($c[0] - .5) ($c[1] - .5) $z0 ($c[0] + .5) ($c[1] + .5) $z1 'H' }
}
$doorL = @(LBox 8 10 2.5 10 13 3 'D')      # hinge at x 10 (the viewer's left)
$doorR = @(LBox 6 10 2.5 8 13 3 'D')       # hinge at x 6
$doorLOpen = @(LBox 10 10 .5 10.5 13 2.5 'D')
$doorROpen = @(LBox 5.5 10 .5 6 13 2.5 'D')
$pig = @(
    (LBox 7 10 1 9 12 3 'P'),
    (LBox 7.5 10.5 .5 8.5 11.5 1 'Q'),
    (LBox 7 12 1.5 7.5 12.5 2 'P'), (LBox 8.5 12 1.5 9 12.5 2 'P')
)
$creeper = @(
    (LBox 7 10 1 9 12.75 3 'G'),
    (LBox 7.25 11.75 .9 7.75 12.25 1 'F'), (LBox 8.25 11.75 .9 8.75 12.25 1 'F'),
    (LBox 7.75 10.75 .9 8.25 11.75 1 'F')
)
$clockShaft = @(LBox 6 6 14 10 10 16 'shaft')

$clockBones = @((Bone 'body' ($clockBody + $roof)), (Bone 'shaft' $clockShaft))
foreach ($k in 0..11) { $clockBones += Bone "hour_$k" (HandCubes (30 * $k) 2 2.25 2.5) }
foreach ($k in 0..11) { $clockBones += Bone "minute_$k" (HandCubes (30 * $k) 3 2.0 2.25) }
$clockBones += Bone 'door_left' $doorL
$clockBones += Bone 'door_right' $doorR
$clockBones += Bone 'door_left_open' $doorLOpen
$clockBones += Bone 'door_right_open' $doorROpen
$clockBones += Bone 'pig' $pig
$clockBones += Bone 'creeper' $creeper
Write-Json (GeoFile @(Geometry 'geometry.levitate.cuckoo_clock' $clockBones)) "$models/cuckoo_clock.geo.json" -Compress

# ---- flywheel (parts in wheel coordinates: p, q in the plane, r along the axis; generators.ps1 style) ------
$regions = @{ main = @(0, 0); second = @(16, 0); third = @(0, 16); shaft = @(16, 16) }
function Part($p0, $p1, $q0, $q1, $r0, $r1, $tex, $rot = 0) {
    [pscustomobject]@{ p0 = [double]$p0; p1 = [double]$p1; q0 = [double]$q0; q1 = [double]$q1; r0 = [double]$r0; r1 = [double]$r1; tex = $tex; rot = [double]$rot }
}
function Turn($part, [int]$quarters) {
    $x = $part
    for ($i = 0; $i -lt (($quarters % 4) + 4) % 4; $i++) { $x = Part (-$x.q1) (-$x.q0) $x.p0 $x.p1 $x.r0 $x.r1 $x.tex $x.rot }
    $x
}
function Spin($part, [double]$deg) {
    $quarters = [int][math]::Round($deg / 90)
    $x = Turn $part $quarters
    $x.rot = $deg - 90 * $quarters
    $x
}
# a cube of `part` along +Y (the kinetics renderer and the block model turn it onto the axis)
function YCube($part) {
    if ($part.rot -ne 0) {
        $pc = ($part.p0 + $part.p1) / 2; $qc = ($part.q0 + $part.q1) / 2
        $t = $part.rot * [math]::PI / 180
        $dp = $pc * [math]::Cos($t) - $qc * [math]::Sin($t) - $pc; $dq = $pc * [math]::Sin($t) + $qc * [math]::Cos($t) - $qc
        $part = Part ($part.p0 + $dp) ($part.p1 + $dp) ($part.q0 + $dq) ($part.q1 + $dq) $part.r0 $part.r1 $part.tex $part.rot
    }
    $sx = $part.p1 - $part.p0; $sy = $part.r1 - $part.r0; $sz = $part.q1 - $part.q0
    $reg = $regions[$part.tex]
    $face = { param($w, $h) [ordered]@{ uv = @($reg[0], $reg[1]); uv_size = @((Rnd ([math]::Min(16, $w))), (Rnd ([math]::Min(16, $h)))) } }
    $cube = [ordered]@{ origin = @((Rnd (-$part.p1)), (Rnd ($part.r0 + 8)), (Rnd $part.q0)); size = @((Rnd $sx), (Rnd $sy), (Rnd $sz)) }
    if ($part.rot -ne 0) {
        $cube.pivot = @((Rnd (-($part.p0 + $part.p1) / 2)), 8, (Rnd (($part.q0 + $part.q1) / 2)))
        $cube.rotation = @(0, (Rnd $part.rot), 0)
    }
    $cube.uv = [ordered]@{
        north = & $face $sx $sy; south = & $face $sx $sy
        east = & $face $sz $sy; west = & $face $sz $sy
        up = & $face $sx $sz; down = & $face $sx $sz
    }
    $cube
}
$wheel = @((Part -3 3 -3 3 -2.5 2.5 third))
$rimSeg = Part 11 14 -2.85 2.85 -2 2 main
foreach ($k in 0..15) { $wheel += Spin $rimSeg (22.5 * $k) }
$spoke = Part 3 11 -1 1 -1.5 1.5 second
foreach ($k in 0..3) { $wheel += Turn $spoke $k; $wheel += Spin (Part 3 11 -.75 .75 -1 1 second) (45 + 90 * $k) }
$flyShaft = @(Part -2 2 -2 2 -8 8 shaft)
Write-Json (GeoFile @(Geometry 'geometry.levitate.flywheel' @((Bone 'wheel' @($wheel | ForEach-Object { YCube $_ })), (Bone 'shaft' @($flyShaft | ForEach-Object { YCube $_ }))))) "$models/flywheel.geo.json" -Compress

# ---- behavior pack -------------------------------------------------------------------------------------------
function BlockFile($id, $category, $states, $components, $permutations, $format = '1.21.70') {
    $desc = [ordered]@{ identifier = "levitate:$id"; menu_category = [ordered]@{ category = $category } }
    $desc.states = $states
    $block = [ordered]@{ description = $desc; components = $components }
    if ($permutations) { $block.permutations = @($permutations) }
    Write-Json ([ordered]@{ format_version = $format; 'minecraft:block' = $block }) "$bp/blocks/$id.json"
}
function Loot($id) { Write-Json ([ordered]@{ pools = @([ordered]@{ rolls = 1; entries = @([ordered]@{ type = 'item'; name = "levitate:$id"; weight = 1 }) }) }) "$bp/loot_tables/blocks/$id.json" }
function Box($o, $s) { [ordered]@{ origin = $o; size = $s } }

# cuckoo clocks: levitate:horizontal_facing 0 south, 1 west, 2 north, 3 east (Java 2D value)
$vis = [ordered]@{ shaft = "q.block_state('levitate:rotor') != 1" }
foreach ($k in 0..11) { $vis["hour_$k"] = "q.block_state('levitate:rotor') != 1 && q.block_state('levitate:hour') == $k" }
foreach ($k in 0..11) { $vis["minute_$k"] = "q.block_state('levitate:rotor') != 1 && q.block_state('levitate:minute') == $k" }
$vis['door_left'] = "q.block_state('levitate:rotor') != 1 && q.block_state('levitate:cuckoo') == 0"
$vis['door_right'] = "q.block_state('levitate:rotor') != 1 && q.block_state('levitate:cuckoo') == 0"
$vis['door_left_open'] = "q.block_state('levitate:rotor') != 1 && q.block_state('levitate:cuckoo') != 0"
$vis['door_right_open'] = "q.block_state('levitate:rotor') != 1 && q.block_state('levitate:cuckoo') != 0"
$vis['pig'] = "q.block_state('levitate:rotor') != 1 && q.block_state('levitate:cuckoo') == 1"
$vis['creeper'] = "q.block_state('levitate:rotor') != 1 && q.block_state('levitate:cuckoo') == 2"
$yaw = @{ 0 = 180; 1 = 90; 2 = 0; 3 = 270 }
foreach ($id in 'cuckoo_clock', 'mysterious_cuckoo_clock') {
    $c = [ordered]@{
        'minecraft:geometry'                  = [ordered]@{ identifier = 'geometry.levitate.cuckoo_clock'; bone_visibility = $vis }
        'minecraft:material_instances'        = [ordered]@{
            '*'   = [ordered]@{ texture = 'levitate_cuckoo_clock'; render_method = 'alpha_test' }
            shaft = [ordered]@{ texture = 'levitate_transmission_rotor'; render_method = 'alpha_test' }
        }
        'minecraft:collision_box'             = Box @(-7, 0, -7) @(14, 16, 14)
        'minecraft:selection_box'             = Box @(-7, 0, -7) @(14, 16, 14)
        'minecraft:destructible_by_mining'    = [ordered]@{ seconds_to_destroy = 1.5 }
        'minecraft:destructible_by_explosion' = [ordered]@{ explosion_resistance = 3 }
        'minecraft:light_dampening'           = 0
        'minecraft:map_color'                 = '#9e6a49'
        'minecraft:loot'                      = "loot_tables/blocks/$id.json"
    }
    $perms = foreach ($f in 0..3) {
        [ordered]@{ condition = "q.block_state('levitate:horizontal_facing') == $f"; components = [ordered]@{ 'minecraft:transformation' = [ordered]@{ rotation = @(0, $yaw[$f], 0) } } }
    }
    $states = [ordered]@{ 'levitate:horizontal_facing' = Range 4; 'levitate:hour' = Range 12; 'levitate:minute' = Range 12; 'levitate:cuckoo' = Range 3; 'levitate:rotor' = Range 2 }
    BlockFile $id 'construction' $states $c $perms
    Loot $id
}

# flywheel: the Y model turned onto levitate:axis (0 x, 1 y, 2 z)
$c = [ordered]@{
    'minecraft:geometry'                  = [ordered]@{ identifier = 'geometry.levitate.flywheel'; bone_visibility = [ordered]@{ wheel = "q.block_state('levitate:rotor') != 1"; shaft = "q.block_state('levitate:rotor') != 1" } }
    'minecraft:material_instances'        = [ordered]@{ '*' = [ordered]@{ texture = 'levitate_flywheel'; render_method = 'alpha_test' } }
    'minecraft:collision_box'             = Box @(-8, 0, -8) @(16, 16, 16)
    'minecraft:selection_box'             = Box @(-8, 0, -8) @(16, 16, 16)
    'minecraft:destructible_by_mining'    = [ordered]@{ seconds_to_destroy = 2.0 }
    'minecraft:destructible_by_explosion' = [ordered]@{ explosion_resistance = 6 }
    'minecraft:light_dampening'           = 0
    'minecraft:map_color'                 = '#9e6a49'
    'minecraft:loot'                      = 'loot_tables/blocks/flywheel.json'
}
$perms = @(
    [ordered]@{ condition = "q.block_state('levitate:axis') == 0"; components = [ordered]@{ 'minecraft:transformation' = [ordered]@{ rotation = @(0, 0, 90) } } },
    [ordered]@{ condition = "q.block_state('levitate:axis') == 2"; components = [ordered]@{ 'minecraft:transformation' = [ordered]@{ rotation = @(90, 0, 0) } } }
)
BlockFile 'flywheel' 'construction' ([ordered]@{ 'levitate:axis' = Range 3; 'levitate:rotor' = Range 2 }) $c $perms
Loot 'flywheel'

# ---- recipes (Create 6.0.10 crafting/kinetics) ---------------------------------------------------------------
function Shaped($id, $pattern, $key, $unlock) {
    $k = [ordered]@{}
    foreach ($e in $key.GetEnumerator() | Sort-Object Key) { $k[$e.Key] = $e.Value }
    Write-Json ([ordered]@{ format_version = '1.20.10'; 'minecraft:recipe_shaped' = [ordered]@{
                description = [ordered]@{ identifier = "levitate:$id" }; tags = @('crafting_table'); pattern = @($pattern); key = $k
                unlock = @([ordered]@{ item = $unlock }); result = [ordered]@{ item = "levitate:$id"; count = 1 }
            } }) "$bp/recipes/$id.json"
}
Shaped 'cuckoo_clock' @('S', 'C', 'A') @{ S = [ordered]@{ tag = 'minecraft:planks' }; C = [ordered]@{ item = 'levitate:andesite_casing' }; A = [ordered]@{ item = 'minecraft:clock' } } 'levitate:andesite_casing'
Shaped 'mysterious_cuckoo_clock' @(' C ', 'CBC', ' C ') @{ B = [ordered]@{ item = 'levitate:cuckoo_clock' }; C = [ordered]@{ item = 'minecraft:gunpowder' } } 'levitate:cuckoo_clock'
Shaped 'flywheel' @('CCC', 'CAC', 'CCC') @{ A = [ordered]@{ item = 'levitate:shaft' }; C = [ordered]@{ item = 'levitate:brass_ingot' } } 'levitate:brass_ingot'
Write-Host "transmission models and blocks written"
