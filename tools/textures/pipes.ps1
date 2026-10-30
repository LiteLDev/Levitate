# Textures, block models and block definitions of the pipes unit (original pixel art and geometry).
#   pwsh tools/textures/pipes.ps1
#
# Writes:
#   assets/pack/rp/textures/blocks/{fluid_pipe,glass_fluid_pipe,encased_fluid_pipe,mechanical_pump,
#       fluid_valve,smart_fluid_pipe,pipe_bracket_wood,pipe_bracket_metal,pipes_test_tank,pipe_fluid_*}.png
#   assets/pack/rp/models/blocks/{fluid_pipe,glass_fluid_pipe,encased_fluid_pipe,mechanical_pump,
#       mechanical_pump_cog,fluid_valve,fluid_valve_shaft,smart_fluid_pipe}.geo.json
#   assets/pack/bp/blocks/<id>.json for the six blocks and the test tank (the bone visibility
#   expressions are generated together with the bones).
#   assets/pack/rp/textures/particle/levitate_fluid.png (the fluid particles, tinted per fluid: four
#   4x4 bits of a light, mottled surface; levitate:fluid_particle picks one, levitate:fluid_drip the
#   first).
#
# Texture layout (16x16, four 8x8 quarters): TL body side, TR end / ring, BL casing / housing,
# BR detail (cog, handle, slot, rim). Models address the quarters by name below.
. "$PSScriptRoot/lib.ps1"
$root    = Resolve-Path "$PSScriptRoot/../.."
$texOut  = Join-Path $root 'assets/pack/rp/textures/blocks'
$geoOut  = Join-Path $root 'assets/pack/rp/models/blocks'
$bpOut   = Join-Path $root 'assets/pack/bp/blocks'

# ---- textures ----------------------------------------------------------------------------------

function Quarters([string[]]$tl, [string[]]$tr, [string[]]$bl, [string[]]$br) {
    $rows = @()
    for ($y = 0; $y -lt 8; $y++) { $rows += $tl[$y] + $tr[$y] }
    for ($y = 0; $y -lt 8; $y++) { $rows += $bl[$y] + $br[$y] }
    return , $rows
}

# copper pipe: body with a highlight line and darker seams; end ring with a dark bore
$pipeSide = @('dddddddd', 'llllllll', 'hhhhhhhh', 'mmmmmmmm', 'mmmmmmmm', 'kkkkkkkk', 'kkkkkkkk', 'dddddddd')
$pipeEnd  = @('dddddddd', 'dhhhhmkd', 'dhoooomd', 'dmoooomd', 'dmoooomd', 'dmooookd', 'dkmmmkkd', 'dddddddd')
$casing   = @('ccccccca', 'cbbbbbba', 'cbmmmmba', 'cbmmmmba', 'cbmmmmba', 'cbmmmmba', 'cbbbbbba', 'aaaaaaaa')
$rimSide  = @('dddddddd', 'hhhhhhhh', 'mmmmmmmm', 'kkkkkkkk', 'hhhhhhhh', 'mmmmmmmm', 'kkkkkkkk', 'dddddddd')
$copper   = @{ d = '#6a3a22'; l = '#f0a77c'; h = '#e2875a'; m = '#c56d43'; k = '#9a5232'; o = '#2a1a12'; c = '#b8693f'; b = '#8c4b2c'; a = '#5b3019' }
Save-PixelArt -Rows (Quarters $pipeSide $pipeEnd $casing $rimSide) -Palette $copper -Path "$texOut/fluid_pipe.png"

# glass pipe: copper frame edges around clear glass with a few glints
$glass = @(
    'dddddddddddddddd',
    'g..............g',
    'g.w............g',
    'g..w...........g',
    'g..............g',
    'g..............g',
    'g.........w....g',
    'g..........w...g',
    'g..............g',
    'g..............g',
    'g....w.........g',
    'g.....w........g',
    'g..............g',
    'g.............wg',
    'g..............g',
    'dddddddddddddddd'
)
Save-PixelArt -Rows $glass -Palette @{ d = '#9a5232'; g = '#c56d43'; w = '#d8f0f4' } -Path "$texOut/glass_fluid_pipe.png"

# fluids seen through glass pipes: diagonal streaks of two tones
$fluidRows = @()
for ($y = 0; $y -lt 16; $y++) {
    $r = ''
    for ($x = 0; $x -lt 16; $x++) { $r += if ((($x + $y) % 6) -lt 2) { 'b' } elseif ((($x - $y + 32) % 9) -eq 0) { 'c' } else { 'a' } }
    $fluidRows += $r
}
$fluids = [ordered]@{
    water     = @('#3d63d8', '#5b83f0', '#2f4fb4')
    lava      = @('#d4581a', '#f6a12a', '#a83a12')
    honey     = @('#e9a12c', '#f8c95a', '#c47f16')
    chocolate = @('#5e321a', '#7a4628', '#45230f')
    milk      = @('#ececec', '#ffffff', '#d2d2d2')
    potion    = @('#7a3fb0', '#9b5fd2', '#5c2a8c')
    tea       = @('#9a5a2c', '#b8733e', '#7a4420')
}
foreach ($name in $fluids.Keys) {
    $c = $fluids[$name]
    Save-PixelArt -Rows $fluidRows -Palette @{ a = $c[0]; b = $c[1]; c = $c[2] } -Path "$texOut/pipe_fluid_$name.png"
}

# fluid particles: light grey mottling, tinted with the fluid's colour by the particle
$particleRows = @(
    'abbacaabbbaacbba',
    'bccbbaabcbbabccb',
    'bccbabbbacbbbccb',
    'abbabaabbbababba',
    'cabbbbcaabbcabba',
    'abccbbaabccbaabc',
    'abccbabbbccbbabb',
    'bbbaacabbbbaacab',
    'abbacaabbbaacbba',
    'bccbbaabcbbabccb',
    'bcabbbbbacbbbcab',
    'abbabaabbbababba',
    'babbbbcaabbcabba',
    'abccbbaabcabaabc',
    'abccbabbbccbbabb',
    'bbbaacabbbbaacab'
)
Save-PixelArt -Rows $particleRows -Palette @{ a = '#d8d8d8'; b = '#f4f4f4'; c = '#ffffff' } -Path (Join-Path $root 'assets/pack/rp/textures/particle/levitate_fluid.png')

# encased pipe: copper casing face (whole texture)
$encased = @(
    'aaaaaaaaaaaaaaaa',
    'abbbbbbbbbbbbbba',
    'abccccccccccccba',
    'abcmmmmmmmmmmcba',
    'abcmhhhhhhhhmcba',
    'abcmh......hmcba',
    'abcmh.ffff.hmcba',
    'abcmh.ffff.hmcba',
    'abcmh.ffff.hmcba',
    'abcmh.ffff.hmcba',
    'abcmh......hmcba',
    'abcmhhhhhhhhmcba',
    'abcmmmmmmmmmmcba',
    'abccccccccccccba',
    'abbbbbbbbbbbbbba',
    'aaaaaaaaaaaaaaaa'
) | ForEach-Object { $_.Replace('.', 'k') }
Save-PixelArt -Rows $encased -Palette @{ a = '#5b3019'; b = '#8c4b2c'; c = '#b8693f'; m = '#c56d43'; h = '#e2875a'; k = '#7a4126'; f = '#9a5232' } -Path "$texOut/encased_fluid_pipe.png"

# mechanical pump: copper pipe, iron flange, andesite housing with an arrow, brass cog
$flange  = @('aaaaaaaa', 'abbbbbba', 'abiiiiba', 'abiooiba', 'abiooiba', 'abiiiiba', 'abbbbbba', 'aaaaaaaa')
$housing = @('ssssssss', 'svvvvvvs', 'svvwwvvs', 'svwwwwvs', 'svvwwvvs', 'svvwwvvs', 'svvvvvvs', 'ssssssss')
$cog     = @('yyyyyyyy', 'yzzzzzzy', 'yzqqqqzy', 'yzqrrqzy', 'yzqrrqzy', 'yzqqqqzy', 'yzzzzzzy', 'yyyyyyyy')
$pumpPal = $copper.Clone()
$pumpPal.a = '#3c3f44'; $pumpPal.b = '#6b7078'; $pumpPal.i = '#9ca3ab'; $pumpPal.o = '#22252a'
$pumpPal.s = '#5d6060'; $pumpPal.v = '#8e928d'; $pumpPal.w = '#e9e2c2'
$pumpPal.y = '#7a5a1e'; $pumpPal.z = '#c49a3a'; $pumpPal.q = '#e2bb5c'; $pumpPal.r = '#5a4116'
Save-PixelArt -Rows (Quarters $pipeSide $flange $housing $cog) -Palette $pumpPal -Path "$texOut/mechanical_pump.png"

# fluid valve: copper pipe, iron body, andesite shaft, red handle
$body   = @('aaaaaaaa', 'abbbbbba', 'abiiiiba', 'abiiiiba', 'abiiiiba', 'abiiiiba', 'abbbbbba', 'aaaaaaaa')
$shaft  = @('svvsvvsv', 'vvsvvsvv', 'svvsvvsv', 'vvsvvsvv', 'svvsvvsv', 'vvsvvsvv', 'svvsvvsv', 'vvsvvsvv')
$handle = @('nnnnnnnn', 'ntttttpn', 'ntppppen', 'ntppppen', 'ntppppen', 'ntppppen', 'neeeeeen', 'nnnnnnnn')
$valvePal = $pumpPal.Clone()
$valvePal.n = '#5a1414'; $valvePal.t = '#e05a4a'; $valvePal.p = '#c23a2c'; $valvePal.e = '#8e2418'
Save-PixelArt -Rows (Quarters $pipeSide $body $shaft $handle) -Palette $valvePal -Path "$texOut/fluid_valve.png"

# smart fluid pipe: copper pipe, brass body, dark filter slot
$brass = @('aaaaaaaa', 'abbbbbba', 'abccccba', 'abcbbcba', 'abcbbcba', 'abccccba', 'abbbbbba', 'aaaaaaaa')
$slot  = @('xxxxxxxx', 'xuuuuuux', 'xujjjjux', 'xujjjjux', 'xujjjjux', 'xujjjjux', 'xuuuuuux', 'xxxxxxxx')
$smartPal = $copper.Clone()
$smartPal.a = '#6e4f12'; $smartPal.b = '#c99a2e'; $smartPal.c = '#e8c25a'
$smartPal.x = '#2b2416'; $smartPal.u = '#4a3f2a'; $smartPal.j = '#1b1710'
Save-PixelArt -Rows (Quarters $pipeSide $pipeEnd $brass $slot) -Palette $smartPal -Path "$texOut/smart_fluid_pipe.png"

# brackets
$bracketRows = @('aaaaaaaaaaaaaaaa', 'abbbbbbbbbbbbbba', 'abccccccccccccba', 'abcbbbbbbbbbbcba') * 4
Save-PixelArt -Rows $bracketRows -Palette @{ a = '#4e3218'; b = '#7a5530'; c = '#9c7244' } -Path "$texOut/pipe_bracket_wood.png"
Save-PixelArt -Rows $bracketRows -Palette @{ a = '#3c3f44'; b = '#6b7078'; c = '#9ca3ab' } -Path "$texOut/pipe_bracket_metal.png"

# test tank (selftests only): glassy iron box
$tankRows = @(
    'aaaaaaaaaaaaaaaa', 'abbbbbbbbbbbbbba', 'ab............ba', 'ab.w..........ba',
    'ab..w.........ba', 'ab............ba', 'ab............ba', 'ab............ba',
    'ab............ba', 'ab............ba', 'ab............ba', 'ab.........w..ba',
    'ab..........w.ba', 'ab............ba', 'abbbbbbbbbbbbbba', 'aaaaaaaaaaaaaaaa'
) | ForEach-Object { $_.Replace('.', 'g') }
Save-PixelArt -Rows $tankRows -Palette @{ a = '#3c3f44'; b = '#6b7078'; g = '#5a7f8a'; w = '#d8f0f4' } -Path "$texOut/pipes_test_tank.png"

# ---- geometry helpers -------------------------------------------------------------------------------

$Q = @{ tl = @(0, 0); tr = @(8, 0); bl = @(0, 8); br = @(8, 8); all = @(0, 0) }

function Faces($quarter, [string]$material, [int]$size = 8) {
    $f = [ordered]@{}
    foreach ($n in 'north', 'south', 'east', 'west', 'up', 'down') {
        $e = [ordered]@{ uv = $Q[$quarter]; uv_size = @($size, $size) }
        if ($material) { $e.material_instance = $material }
        $f[$n] = $e
    }
    return $f
}

function Cube([double[]]$origin, [double[]]$size, $quarter = 'tl', [string]$material = $null, [int]$uvSize = 8) {
    return [ordered]@{ origin = $origin; size = $size; uv = (Faces $quarter $material $uvSize) }
}

function Bone([string]$name, $cubes) { return [ordered]@{ name = $name; pivot = @(0, 8, 0); cubes = @($cubes) } }

function Save-Geo([string]$id, [string]$file, $bones) {
    $geo = [ordered]@{
        format_version      = '1.16.0'
        'minecraft:geometry' = @([ordered]@{
                description = [ordered]@{
                    identifier            = $id
                    texture_width         = 16
                    texture_height        = 16
                    visible_bounds_width  = 2
                    visible_bounds_height = 2
                    visible_bounds_offset = @(0, 0.5, 0)
                }
                bones       = @($bones)
            })
    }
    $geo | ConvertTo-Json -Depth 30 | Set-Content -Encoding utf8NoBOM (Join-Path $geoOut $file)
}

# Integer rotation of a cube about the block centre (0, 8, 0): $m maps canonical (x, y, z) to world.
function Rot($cube, [int[]]$m) {
    $o = $cube.origin; $s = $cube.size
    $a = @(($o[0]), ($o[1] - 8), ($o[2])); $b = @(($o[0] + $s[0]), ($o[1] + $s[1] - 8), ($o[2] + $s[2]))
    $ta = @(0, 0, 0); $tb = @(0, 0, 0)
    for ($i = 0; $i -lt 3; $i++) {
        $ta[$i] = $m[$i * 3] * $a[0] + $m[$i * 3 + 1] * $a[1] + $m[$i * 3 + 2] * $a[2]
        $tb[$i] = $m[$i * 3] * $b[0] + $m[$i * 3 + 1] * $b[1] + $m[$i * 3 + 2] * $b[2]
    }
    $min = @(); $sz = @()
    for ($i = 0; $i -lt 3; $i++) { $min += [Math]::Min($ta[$i], $tb[$i]); $sz += [Math]::Abs($tb[$i] - $ta[$i]) }
    $min[1] += 8
    return [ordered]@{ origin = $min; size = $sz; uv = $cube.uv }
}

# every rotation (signed permutation matrix with determinant +1)
$Rotations = @()
foreach ($perm in @(@(0, 1, 2), @(0, 2, 1), @(1, 0, 2), @(1, 2, 0), @(2, 0, 1), @(2, 1, 0))) {
    foreach ($sx in 1, -1) { foreach ($sy in 1, -1) { foreach ($sz in 1, -1) {
                $m = @(0, 0, 0, 0, 0, 0, 0, 0, 0); $sg = @($sx, $sy, $sz)
                for ($r = 0; $r -lt 3; $r++) { $m[$r * 3 + $perm[$r]] = $sg[$r] }
                $det = $m[0] * ($m[4] * $m[8] - $m[5] * $m[7]) - $m[1] * ($m[3] * $m[8] - $m[5] * $m[6]) + $m[2] * ($m[3] * $m[7] - $m[4] * $m[6])
                if ($det -eq 1) { $Rotations += , $m }
            } } }
}
function Apply([int[]]$m, [int[]]$v) { return @(($m[0] * $v[0] + $m[1] * $v[1] + $m[2] * $v[2]), ($m[3] * $v[0] + $m[4] * $v[1] + $m[5] * $v[2]), ($m[6] * $v[0] + $m[7] * $v[1] + $m[8] * $v[2])) }
function Same([int[]]$a, [int[]]$b) { return ($a[0] -eq $b[0]) -and ($a[1] -eq $b[1]) -and ($a[2] -eq $b[2]) }
# a rotation taking canonical vector $from to $to and $from2 to $to2 (or its opposite when $either)
function Find-Rotation([int[]]$from, [int[]]$to, [int[]]$from2, [int[]]$to2, [switch]$Either) {
    foreach ($m in $Rotations) {
        if (-not (Same (Apply $m $from) $to)) { continue }
        $v = Apply $m $from2
        if ((Same $v $to2) -or ($Either -and (Same $v @(-$to2[0], -$to2[1], -$to2[2])))) { return , $m }
    }
    throw "no rotation"
}

# a rotation taking canonical +y to $to (canonical +x goes to x, z or y, whichever is free)
function Turn([int[]]$to) {
    foreach ($t in @(@(1, 0, 0), @(0, 0, 1), @(0, 1, 0))) {
        try { return , (Find-Rotation @(0, 1, 0) $to @(1, 0, 0) $t -Either) } catch {}
    }
    throw "no rotation to $to"
}
function Merge($a, $b) { $o = [ordered]@{}; foreach ($k in $a.Keys) { $o[$k] = $a[$k] }; foreach ($k in $b.Keys) { $o[$k] = $b[$k] }; return $o }

# faces in Bedrock FacingID order with their state names and vectors
$Faces = @(
    @{ i = 0; n = 'down'; v = @(0, -1, 0) }, @{ i = 1; n = 'up'; v = @(0, 1, 0) },
    @{ i = 2; n = 'north'; v = @(0, 0, -1) }, @{ i = 3; n = 'south'; v = @(0, 0, 1) },
    @{ i = 4; n = 'west'; v = @(-1, 0, 0) }, @{ i = 5; n = 'east'; v = @(1, 0, 0) }
)
$AxisVec = @(@(1, 0, 0), @(0, 1, 0), @(0, 0, 1))
function S([string]$n) { return "q.block_state('levitate:$n')" }

# ---- fluid pipe -------------------------------------------------------------------------------------
# canonical arm towards +y: from the core (y 12) to the face (y 16)
$bones = @()
$vis   = [ordered]@{}
$bones += Bone 'core' @(Cube @(-4, 4, -4) @(8, 8, 8) 'tl')
foreach ($f in $Faces) {
    $m = Turn $f.v
    $bones += Bone "arm_$($f.n)" @(Rot (Cube @(-4, 12, -4) @(8, 4, 8) 'tl') $m)
    $bones += Bone "rim_$($f.n)" @(Rot (Cube @(-5, 14, -5) @(10, 2, 10) 'br') $m)
    $vis["arm_$($f.n)"] = "$(S $f.n) == 1"
    $state = if ($f.i -lt 3) { 'rim_a' } else { 'rim_b' }
    $bitv  = [int][Math]::Pow(2, $f.i % 3)
    $vis["rim_$($f.n)"] = "math.mod(math.floor($(S $state) / $bitv), 2) == 1"
}
$bones += Bone 'casing' @(Cube @(-5, 3, -5) @(10, 10, 10) 'bl')
$vis['casing'] = "($(S north) + $(S south) + $(S west) + $(S east) >= 3) || ($(S up) + $(S down) + $(S north) + $(S south) >= 3) || ($(S up) + $(S down) + $(S west) + $(S east) >= 3)"
$straight = @{
    0 = "$(S east) + $(S west) == 2 && $(S up) + $(S down) + $(S north) + $(S south) == 0"
    1 = "$(S up) + $(S down) == 2 && $(S east) + $(S west) + $(S north) + $(S south) == 0"
    2 = "$(S north) + $(S south) == 2 && $(S up) + $(S down) + $(S east) + $(S west) == 0"
}
function Bracket-Bones([string]$prefix, $straightFor, $vis) {
    $out = @()
    foreach ($ax in 0, 1, 2) {
        $m = Turn $AxisVec[$ax]
        foreach ($kind in @(@{ n = 'wood'; v = 1 }, @{ n = 'metal'; v = 2 })) {
            $cubes = @(
                (Rot (Cube @(-5, 6, -5) @(10, 4, 10) 'all' "bracket_$($kind.n)" 16) $m),
                (Rot (Cube @(-1, 6, -8) @(2, 4, 3) 'all' "bracket_$($kind.n)" 16) $m)
            )
            $name = "${prefix}bracket_$($ax)_$($kind.n)"
            $out += Bone $name $cubes
            $vis[$name] = "$(S bracket) == $($kind.v) && ($($straightFor[$ax]))"
        }
    }
    return , $out
}
$bones += Bracket-Bones '' $straight $vis
Save-Geo 'geometry.levitate.fluid_pipe' 'fluid_pipe.geo.json' $bones
$pipeVis = $vis

# ---- glass fluid pipe --------------------------------------------------------------------------------
$bones = @()
$vis   = [ordered]@{}
$glassAxis = @{ 0 = "$(S axis) == 0"; 1 = "$(S axis) == 1"; 2 = "$(S axis) == 2" }
$slots = @('', 'water', 'lava', 'honey', 'chocolate', 'milk', 'potion', 'tea')
foreach ($ax in 0, 1, 2) {
    $m = Turn $AxisVec[$ax]
    $bones += Bone "body_$ax" @((Rot (Cube @(-4, 0, -4) @(8, 16, 8) 'all' $null 16) $m))
    $bones += Bone "ends_$ax" @(
        (Rot (Cube @(-5, 0, -5) @(10, 2, 10) 'br' 'copper') $m),
        (Rot (Cube @(-5, 14, -5) @(10, 2, 10) 'br' 'copper') $m)
    )
    $vis["body_$ax"] = $glassAxis[$ax]
    $vis["ends_$ax"] = $glassAxis[$ax]
    for ($s = 1; $s -lt $slots.Count; $s++) {
        $name = "fluid_$($ax)_$s"
        $bones += Bone $name @((Rot (Cube @(-3, 2, -3) @(6, 12, 6) 'all' "fluid_$($slots[$s])" 16) $m))
        $vis[$name] = "$($glassAxis[$ax]) && $(S fluid) == $s"
    }
}
$bones += Bracket-Bones '' $glassAxis $vis
Save-Geo 'geometry.levitate.glass_fluid_pipe' 'glass_fluid_pipe.geo.json' $bones
$glassVis = $vis

# ---- encased fluid pipe ------------------------------------------------------------------------------
$bones = @(Bone 'casing' @(Cube @(-8, 0, -8) @(16, 16, 16) 'all' $null 16))
$vis   = [ordered]@{}
foreach ($f in $Faces) {
    $m = Turn $f.v
    $bones += Bone "port_$($f.n)" @(Rot (Cube @(-4, 16, -4) @(8, 0.5, 8) 'tr' 'port') $m)
    $vis["port_$($f.n)"] = "$(S $f.n) == 1"
}
Save-Geo 'geometry.levitate.encased_fluid_pipe' 'encased_fluid_pipe.geo.json' $bones
$encasedVis = $vis

# ---- mechanical pump ----------------------------------------------------------------------------------
# canonical: facing +y (arrow up), arrow on the +x side of the housing
$cogCubes = @(
    (Cube @(-5, 6.5, -5) @(10, 3, 10) 'br'),
    (Cube @(-7, 7, -1.5) @(14, 2, 3) 'br'),
    (Cube @(-1.5, 7, -7) @(3, 2, 14) 'br')
)
$pumpStatic = @(
    (Cube @(-4, 0, -4) @(8, 16, 8) 'tl'),
    (Cube @(-5, 0, -5) @(10, 2, 10) 'tr'),
    (Cube @(-5, 14, -5) @(10, 2, 10) 'tr'),
    (Cube @(-6, 2, -6) @(12, 4, 12) 'bl'),
    (Cube @(-6, 10, -6) @(12, 4, 12) 'bl'),
    (Cube @(6, 10.5, -0.5) @(0.5, 2, 1) 'br'),
    (Cube @(6, 12.5, -1.5) @(0.5, 0.5, 3) 'br'),
    (Cube @(6, 13, -0.5) @(0.5, 0.5, 1) 'br')
)
$bones = @()
$vis   = [ordered]@{}
foreach ($f in $Faces) {
    $m = Turn $f.v
    $bones += Bone "pump_$($f.n)" @($pumpStatic | ForEach-Object { Rot $_ $m })
    $bones += Bone "cog_$($f.n)" @($cogCubes | ForEach-Object { Rot $_ $m })
    $vis["pump_$($f.n)"] = "$(S facing) == $($f.i)"
    $vis["cog_$($f.n)"]  = "$(S facing) == $($f.i) && $(S rotor) != 1"
}
Save-Geo 'geometry.levitate.mechanical_pump' 'mechanical_pump.geo.json' $bones
$pumpVis = $vis
# the rotating part for the client mod: the cog around y through the block centre
$rotorCog = @(
    (Cube @(-5, 6.5, -5) @(10, 3, 10) 'br'),
    (Cube @(-7, 7, -1.5) @(14, 2, 3) 'br'),
    (Cube @(-1.5, 7, -7) @(3, 2, 14) 'br')
)
Save-Geo 'geometry.levitate.mechanical_pump_cog' 'mechanical_pump_cog.geo.json' @(Bone 'rotor' $rotorCog)

# ---- fluid valve ----------------------------------------------------------------------------------------
# canonical: facing +y (handle on top), pipe along x, shaft along z
function ValveAxes([int]$facing, [int]$along) {
    $fa = if ($facing -lt 2) { 1 } elseif ($facing -lt 4) { 2 } else { 0 }
    switch ($fa) {
        0 { if ($along) { return @(2, 1) } else { return @(1, 2) } }
        1 { if ($along) { return @(2, 0) } else { return @(0, 2) } }
        2 { if ($along) { return @(1, 0) } else { return @(0, 1) } }
    }
}
$bones = @()
$vis   = [ordered]@{}
foreach ($f in $Faces) {
    foreach ($along in 0, 1) {
        $axes = ValveAxes $f.i $along
        $m    = Find-Rotation @(0, 1, 0) $f.v @(1, 0, 0) $AxisVec[$axes[0]] -Either
        $cond = "$(S facing) == $($f.i) && $(S along_first) == $along"
        $tag  = "$($f.n)_$along"
        $bones += Bone "valve_$tag" @(
            (Rot (Cube @(-8, 4, -4) @(16, 8, 8) 'tl') $m),
            (Rot (Cube @(-5, 3, -5) @(10, 10, 10) 'tr') $m),
            (Rot (Cube @(-1, 13, -1) @(2, 2, 2) 'br') $m)
        )
        $bones += Bone "handle_open_$tag" @((Rot (Cube @(-5, 14, -1) @(10, 1, 2) 'br') $m))
        $bones += Bone "handle_closed_$tag" @((Rot (Cube @(-1, 14, -5) @(2, 1, 10) 'br') $m))
        $bones += Bone "shaft_$tag" @((Rot (Cube @(-2, 6, -8) @(4, 4, 16) 'bl') $m))
        $vis["valve_$tag"]         = $cond
        $vis["handle_open_$tag"]   = "$cond && $(S enabled) == 1"
        $vis["handle_closed_$tag"] = "$cond && $(S enabled) == 0"
        $vis["shaft_$tag"]         = "$cond && $(S rotor) != 1"
    }
}
Save-Geo 'geometry.levitate.fluid_valve' 'fluid_valve.geo.json' $bones
$valveVis = $vis
Save-Geo 'geometry.levitate.fluid_valve_shaft' 'fluid_valve_shaft.geo.json' @(Bone 'rotor' @(Cube @(-2, 0, -2) @(4, 16, 4) 'bl'))

# ---- smart fluid pipe --------------------------------------------------------------------------------
# filter slot position as Create's SmartPipeFilterSlot (voxels), turned by the facing's angle
$hAngle = @{ 2 = 180; 3 = 0; 4 = 90; 5 = 270 }   # north, south, west, east
$bones = @()
$vis   = [ordered]@{}
foreach ($face in 0, 1, 2) {
    foreach ($fs in 0, 1, 2, 3) {
        $facing = $fs + 2
        $axis   = if ($face -eq 2) { 1 } elseif ($facing -lt 4) { 2 } else { 0 }
        $m      = Turn $AxisVec[$axis]
        switch ($face) { 1 { $y = 0.55; $z = 4.6 } 2 { $y = 11.4; $z = 0.55 } default { $y = 15.45; $z = 4.625 } }
        $angle = $hAngle[$facing] + $(if ($face -eq 2) { 180 } else { 0 })
        $rad   = $angle * [Math]::PI / 180
        $z0    = $z - 8
        $sx    = [Math]::Round([Math]::Sin($rad) * $z0, 2)
        $sz    = [Math]::Round([Math]::Cos($rad) * $z0, 2)
        $plate = switch ($face) {
            0 { Cube @(($sx - 2.5), 13, ($sz - 2.5)) @(5, 0.5, 5) 'br' }
            1 { Cube @(($sx - 2.5), 2.5, ($sz - 2.5)) @(5, 0.5, 5) 'br' }
            default {
                if ([Math]::Abs($sx) -gt [Math]::Abs($sz)) { $px = if ($sx -gt 0) { 5 } else { -5.5 }; Cube @($px, 9, -2.5) @(0.5, 5, 5) 'br' }
                else { $pz = if ($sz -gt 0) { 5 } else { -5.5 }; Cube @(-2.5, 9, $pz) @(5, 5, 0.5) 'br' }
            }
        }
        $tag = "$($face)_$fs"
        $bones += Bone "smart_$tag" @(
            (Rot (Cube @(-4, 0, -4) @(8, 16, 8) 'tl') $m),
            (Rot (Cube @(-5, 0, -5) @(10, 2, 10) 'tr') $m),
            (Rot (Cube @(-5, 14, -5) @(10, 2, 10) 'tr') $m),
            (Cube @(-5, 3, -5) @(10, 10, 10) 'bl'),
            $plate
        )
        $vis["smart_$tag"] = "$(S face) == $face && $(S facing) == $fs"
    }
}
Save-Geo 'geometry.levitate.smart_fluid_pipe' 'smart_fluid_pipe.geo.json' $bones
$smartVis = $vis

# ---- block definitions ------------------------------------------------------------------------------
function Box([double[]]$o, [double[]]$s) { return [ordered]@{ origin = $o; size = $s } }
function Range([int]$n) { return @(0..($n - 1)) }

function Save-Block([string]$path, $states, [string]$geo, $vis, $materials, $box, [double]$seconds, [string]$category = 'items', $permutations = @(), [switch]$Hidden, [switch]$Waterlogged) {
    $desc = [ordered]@{ identifier = "levitate:$path"; menu_category = [ordered]@{ category = $(if ($Hidden) { 'none' } else { $category }) }; states = $states }
    $components = [ordered]@{
        'minecraft:geometry'                = [ordered]@{ identifier = $geo; bone_visibility = $vis }
        'minecraft:material_instances'      = $materials
        'minecraft:collision_box'           = $box
        'minecraft:selection_box'           = $box
        'minecraft:destructible_by_mining'  = [ordered]@{ seconds_to_destroy = $seconds }
        'minecraft:destructible_by_explosion' = [ordered]@{ explosion_resistance = 6 }
        'minecraft:light_dampening'         = 0
        'minecraft:map_color'               = '#c56d43'
        'minecraft:loot'                    = "loot_tables/blocks/$path.json"
    }
    # Create's SimpleWaterloggedBlock / ProperWaterloggedBlock: the block keeps the water it is placed in
    if ($Waterlogged) {
        $components['minecraft:liquid_detection'] = [ordered]@{
            detection_rules = @([ordered]@{ liquid_type = 'water'; can_contain_liquid = $true; on_liquid_touches = 'no_reaction' })
        }
    }
    $block = [ordered]@{ format_version = '1.21.70'; 'minecraft:block' = [ordered]@{ description = $desc; components = $components } }
    if ($permutations.Count -gt 0) { $block['minecraft:block'].permutations = @($permutations) }
    $block | ConvertTo-Json -Depth 30 | Set-Content -Encoding utf8NoBOM (Join-Path $bpOut "$path.json")
}

function Mat([string]$tex, [string]$method = 'alpha_test') { return [ordered]@{ texture = $tex; render_method = $method } }
$brackets = [ordered]@{ bracket_wood = (Mat 'levitate_pipe_bracket_wood'); bracket_metal = (Mat 'levitate_pipe_bracket_metal') }
$core = Box @(-4, 4, -4) @(8, 8, 8)
$axisBox = @{ 0 = (Box @(-8, 4, -4) @(16, 8, 8)); 1 = (Box @(-4, 0, -4) @(8, 16, 8)); 2 = (Box @(-4, 4, -8) @(8, 8, 16)) }

# fluid pipe: six connections, rims and the bracket
$states = [ordered]@{}
foreach ($f in $Faces) { $states["levitate:$($f.n)"] = @(0, 1) }
$states['levitate:rim_a'] = Range 8
$states['levitate:rim_b'] = Range 8
$states['levitate:bracket'] = Range 3
$mats = Merge ([ordered]@{ '*' = (Mat 'levitate_fluid_pipe') }) $brackets
$perms = @()
foreach ($ax in 0, 1, 2) { $perms += [ordered]@{ condition = $straight[$ax]; components = [ordered]@{ 'minecraft:collision_box' = $axisBox[$ax]; 'minecraft:selection_box' = $axisBox[$ax] } } }
Save-Block 'fluid_pipe' $states 'geometry.levitate.fluid_pipe' $pipeVis $mats $core 0.5 'items' $perms -Waterlogged

# glass fluid pipe (no item of its own: it drops a fluid pipe)
$states = [ordered]@{ 'levitate:axis' = (Range 3); 'levitate:fluid' = (Range 8); 'levitate:bracket' = (Range 3) }
$mats = [ordered]@{ '*' = (Mat 'levitate_glass_fluid_pipe' 'blend'); copper = (Mat 'levitate_fluid_pipe' 'blend'); bracket_wood = (Mat 'levitate_pipe_bracket_wood' 'blend'); bracket_metal = (Mat 'levitate_pipe_bracket_metal' 'blend') }
for ($s = 1; $s -lt $slots.Count; $s++) { $mats["fluid_$($slots[$s])"] = Mat "levitate_pipe_fluid_$($slots[$s])" 'blend' }
$perms = @()
foreach ($ax in 0, 1, 2) { $perms += [ordered]@{ condition = $glassAxis[$ax]; components = [ordered]@{ 'minecraft:collision_box' = $axisBox[$ax]; 'minecraft:selection_box' = $axisBox[$ax] } } }
Save-Block 'glass_fluid_pipe' $states 'geometry.levitate.glass_fluid_pipe' $glassVis $mats $axisBox[1] 0.5 'items' $perms -Hidden -Waterlogged

# encased fluid pipe
$states = [ordered]@{}
foreach ($f in $Faces) { $states["levitate:$($f.n)"] = @(0, 1) }
$mats = [ordered]@{ '*' = (Mat 'levitate_encased_fluid_pipe' 'opaque'); port = (Mat 'levitate_fluid_pipe' 'opaque') }
Save-Block 'encased_fluid_pipe' $states 'geometry.levitate.encased_fluid_pipe' $encasedVis $mats (Box @(-8, 0, -8) @(16, 16, 16)) 1.5 'items' @() -Hidden

# mechanical pump
$states = [ordered]@{ 'levitate:facing' = (Range 6); 'levitate:rotor' = (Range 3) }
$mats = [ordered]@{ '*' = (Mat 'levitate_mechanical_pump') }
Save-Block 'mechanical_pump' $states 'geometry.levitate.mechanical_pump' $pumpVis $mats (Box @(-6, 2, -6) @(12, 12, 12)) 1.5 -Waterlogged

# fluid valve
$states = [ordered]@{ 'levitate:facing' = (Range 6); 'levitate:along_first' = @(0, 1); 'levitate:enabled' = @(0, 1); 'levitate:rotor' = (Range 3) }
$mats = [ordered]@{ '*' = (Mat 'levitate_fluid_valve') }
Save-Block 'fluid_valve' $states 'geometry.levitate.fluid_valve' $valveVis $mats (Box @(-5, 3, -5) @(10, 10, 10)) 1.5 -Waterlogged

# smart fluid pipe
$states = [ordered]@{ 'levitate:face' = (Range 3); 'levitate:facing' = (Range 4) }
$mats = [ordered]@{ '*' = (Mat 'levitate_smart_fluid_pipe') }
Save-Block 'smart_fluid_pipe' $states 'geometry.levitate.smart_fluid_pipe' $smartVis $mats (Box @(-5, 3, -5) @(10, 10, 10)) 1.5 -Waterlogged

# the selftests' tank (full cube, not in the creative menu)
$tank = [ordered]@{
    format_version    = '1.21.70'
    'minecraft:block' = [ordered]@{
        description = [ordered]@{ identifier = 'levitate:pipes_test_tank'; menu_category = [ordered]@{ category = 'none' } }
        components  = [ordered]@{
            'minecraft:geometry'                  = 'minecraft:geometry.full_block'
            'minecraft:material_instances'        = [ordered]@{ '*' = (Mat 'levitate_pipes_test_tank' 'opaque') }
            'minecraft:destructible_by_mining'    = [ordered]@{ seconds_to_destroy = 1 }
            'minecraft:destructible_by_explosion' = [ordered]@{ explosion_resistance = 6 }
            'minecraft:loot'                      = 'loot_tables/blocks/pipes_test_tank.json'
        }
    }
}
$tank | ConvertTo-Json -Depth 30 | Set-Content -Encoding utf8NoBOM (Join-Path $bpOut 'pipes_test_tank.json')
Write-Host 'pipes: textures, models and blocks written'
