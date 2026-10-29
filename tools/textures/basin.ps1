# Textures and block models of the basin unit (U14): basin, mechanical press, mechanical mixer,
# blaze burners. Original pixel art and geometry (nothing taken from Create).
#
#   pwsh tools/textures/basin.ps1            # textures -> assets/pack/rp/textures/blocks/, models -> assets/pack/rp/models/blocks/
#   pwsh tools/textures/basin.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
#
# Models are generated from cube lists with per-face UVs taken from the cube's position (like a
# Java block model's automatic UVs) and a material instance per face, so every face samples the
# matching part of a 16x16 texture.
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$blocks = Join-Path $root 'assets/pack/rp/textures/blocks'
$models = Join-Path $root 'assets/pack/rp/models/blocks'

# --- canvas helpers (16x16 char grids) --------------------------------------------------------------
function New-Canvas([char]$fill = '.') { , (0..15 | ForEach-Object { , ([char[]]((New-Object string $fill, 16))) }) }
function Set-Px($c, $x, $y, [char]$k) { if ($x -ge 0 -and $x -lt 16 -and $y -ge 0 -and $y -lt 16) { $c[$y][$x] = $k } }
function Fill($c, $x0, $y0, $x1, $y1, [char]$k) { for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y $k } } }
function Frame($c, $x0, $y0, $x1, $y1, [char]$hi, [char]$lo) {
    for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y0 $hi; Set-Px $c $x $y1 $lo }
    for ($y = $y0; $y -le $y1; $y++) { Set-Px $c $x0 $y $hi; Set-Px $c $x1 $y $lo }
}
function Noise($c, [char]$base, [char]$alt, $seed, $x0 = 0, $y0 = 0, $x1 = 15, $y1 = 15) {
    for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) {
        $h = (($x * 73856093) -bxor ($y * 19349663) -bxor ($seed * 83492791)) -band 0x7fffffff
        Set-Px $c $x $y ($(if ($h % 7 -eq 0) { $alt } else { $base }))
    } }
}
function Rows($c) { , ($c | ForEach-Object { -join $_ }) }
function Save($c, $pal, $name) {
    $rows = Rows $c
    Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $blocks "$name.png")
    if ($Preview) { Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $root "tools/textures/preview/$name.png") -Scale 8 }
}

# --- palettes -------------------------------------------------------------------------------------------
# andesite-alloy grey with warm rivets (basin, press and mixer casings)
$metal = @{ a = '#a7aba3'; b = '#8e928a'; c = '#74786f'; d = '#5a5d56'; e = '#43463f'; h = '#c9ccc4'; r = '#b58c4f'; q = '#7c5d30'; k = '#2b2c28' }
# blaze burner iron cage
$iron = @{ a = '#9ea2a6'; b = '#7d8186'; c = '#5f6368'; d = '#44474b'; e = '#2c2e31'; h = '#c4c8cc'; k = '#18191b'; o = '#6b3a1c' }

# --- basin ------------------------------------------------------------------------------------------------
$c = New-Canvas; Noise $c 'b' 'c' 1; Frame $c 0 0 15 15 'h' 'd'
Fill $c 0 0 15 1 'a'; Fill $c 0 1 15 1 'h'                       # rim band at the top
Fill $c 0 13 15 15 'c'; Fill $c 0 15 15 15 'e'                   # foot band
foreach ($x in @(2, 13)) { Set-Px $c $x 3 'r'; Set-Px $c $x 12 'q' }
Save $c $metal 'basin_side'
$c = New-Canvas; Noise $c 'c' 'd' 2; Frame $c 0 0 15 15 'h' 'e'
Fill $c 2 2 13 13 'e'; Frame $c 2 2 13 13 'd' 'k'                 # rim seen from above, inside dark
Save $c $metal 'basin_top'
$c = New-Canvas; Noise $c 'd' 'c' 3; for ($y = 0; $y -lt 16; $y += 4) { Fill $c 0 $y 15 $y 'e' }
Save $c $metal 'basin_inner'
$c = New-Canvas; Noise $c 'c' 'd' 4; Frame $c 0 0 15 15 'b' 'e'; Fill $c 6 6 9 9 'e'
Save $c $metal 'basin_bottom'

# --- mechanical press ---------------------------------------------------------------------------------------
$casing = @{ a = '#a7aba3'; b = '#8e928a'; c = '#74786f'; d = '#5a5d56'; e = '#43463f'; h = '#c9ccc4'; w = '#9c7442'; v = '#6e4f2b'; k = '#2b2c28'; s = '#bfa36a' }
$c = New-Canvas; Noise $c 'w' 'v' 5                                # wooden panel framed by andesite
Frame $c 0 0 15 15 'h' 'd'; Fill $c 1 1 14 1 'a'; Fill $c 1 14 14 14 'c'
Fill $c 6 6 9 9 'k'; Frame $c 5 5 10 10 'c' 'e'                    # shaft hole
Save $c $casing 'mechanical_press_side'
$c = New-Canvas; Noise $c 'b' 'c' 6; Frame $c 0 0 15 15 'h' 'd'; Frame $c 2 2 13 13 'c' 'a'
Save $c $casing 'mechanical_press_top'
$c = New-Canvas; Noise $c 'c' 'd' 7; Frame $c 0 0 15 15 'b' 'e'; Fill $c 4 4 11 11 'k'
Save $c $casing 'mechanical_press_bottom'
$c = New-Canvas; Noise $c 'a' 'b' 8; Frame $c 0 0 15 15 'h' 'd'      # steel head, a bright pressing face
for ($i = 3; $i -le 12; $i += 3) { Fill $c $i 3 $i 12 'c' }
Save $c $casing 'mechanical_press_head'
$shaft = @{ a = '#b9b9b0'; b = '#9b9b92'; c = '#7d7d75'; d = '#5d5d57'; k = '#3a3a35' }
$c = New-Canvas; for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
    $k = switch ($x % 4) { 0 { 'a' } 1 { 'b' } 2 { 'c' } default { 'd' } }; Set-Px $c $x $y $k } }
for ($y = 0; $y -lt 16; $y += 5) { Fill $c 0 $y 15 $y 'k' }
Save $c $shaft 'mechanical_press_shaft'

# --- mechanical mixer ----------------------------------------------------------------------------------------
$c = New-Canvas; Noise $c 'w' 'v' 9; Frame $c 0 0 15 15 'h' 'd'; Fill $c 1 1 14 1 'a'; Fill $c 1 14 14 14 'c'
Fill $c 3 4 12 5 'c'; Fill $c 3 10 12 11 'c'                       # slats
Save $c $casing 'mechanical_mixer_side'
$c = New-Canvas; Noise $c 'b' 'c' 10; Frame $c 0 0 15 15 'h' 'd'; Fill $c 6 6 9 9 'e'; Frame $c 5 5 10 10 'c' 'a'
Save $c $casing 'mechanical_mixer_top'
$c = New-Canvas; Noise $c 'c' 'd' 11; Frame $c 0 0 15 15 'b' 'e'; Fill $c 6 6 9 9 'k'
Save $c $casing 'mechanical_mixer_bottom'
$cog = @{ a = '#b8875a'; b = '#9c6f45'; c = '#7d5534'; d = '#5b3c23'; k = '#3a2615'; h = '#d3a476' }
$c = New-Canvas; Noise $c 'b' 'c' 12; for ($x = 0; $x -lt 16; $x += 4) { Fill $c $x 0 $x 15 'd' }
Fill $c 0 0 15 0 'h'; Fill $c 0 15 15 15 'k'
Save $c $cog 'mechanical_mixer_cog'
$whisk = @{ a = '#d7dad3'; b = '#b4b8b0'; c = '#8f938b'; d = '#686b64'; k = '#3d3f3a' }
$c = New-Canvas '.'; for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
    if ((($x + $y) % 5) -eq 0 -or (($x - $y + 16) % 5) -eq 0) { Set-Px $c $x $y 'b' } } }
Frame $c 0 0 15 15 'a' 'c'
Save $c $whisk 'mechanical_mixer_whisk'
$c = New-Canvas; for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
    $k = switch ($x % 4) { 0 { 'a' } 1 { 'b' } 2 { 'c' } default { 'd' } }; Set-Px $c $x $y $k } }
Save $c $whisk 'mechanical_mixer_pole'

# --- blaze burners -------------------------------------------------------------------------------------------
$c = New-Canvas '.'                                                 # cage: bars with gaps
for ($x = 0; $x -lt 16; $x++) { for ($y = 0; $y -lt 16; $y++) { if ($x % 3 -eq 0) { Set-Px $c $x $y 'b' } } }
Fill $c 0 0 15 1 'a'; Fill $c 0 14 15 15 'c'; Fill $c 0 0 15 0 'h'; Fill $c 0 15 15 15 'd'
Save $c $iron 'blaze_burner_cage'
$c = New-Canvas; Noise $c 'c' 'd' 13; Frame $c 0 0 15 15 'b' 'e'      # base plate with embers
foreach ($p in @(@(4, 5), @(9, 7), @(6, 10), @(11, 11), @(7, 3))) { Set-Px $c $p[0] $p[1] 'o' }
Save $c $iron 'blaze_burner_base'
$c = New-Canvas; Noise $c 'c' 'b' 14; Frame $c 0 0 15 15 'a' 'e'; Fill $c 3 3 12 12 'd'
Save $c $iron 'blaze_burner_top'

# the blaze head: face (front) and plain sides, per heat
function Blaze-Face($c, [switch]$Front) {
    Noise $c 'y' 'z' 15
    Frame $c 0 0 15 15 'z' 'o'
    if ($Front) { Fill $c 3 6 6 7 'k'; Fill $c 9 6 12 7 'k'; Set-Px $c 4 6 'e'; Set-Px $c 10 6 'e'; Fill $c 5 11 10 11 'o' }
}
$heats = @{
    smouldering = @{ y = '#8a7a52'; z = '#6f5f3c'; o = '#4a3b22'; k = '#1e1a12'; e = '#c9a65a' }
    kindled     = @{ y = '#f2c14e'; z = '#e39a2d'; o = '#a8551a'; k = '#2a1608'; e = '#fff2b0' }
    seething    = @{ y = '#7fd6f0'; z = '#46a6d8'; o = '#245e9a'; k = '#0b1a2e'; e = '#f0fbff' }
}
foreach ($h in $heats.Keys) {
    $c = New-Canvas; Blaze-Face $c -Front; Save $c $heats[$h] "blaze_burner_head_${h}_front"
    $c = New-Canvas; Blaze-Face $c; Save $c $heats[$h] "blaze_burner_head_$h"
}
# flames (cross planes): regular, soul and seething
function Flame($c) {
    for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        $w = [math]::Max(0, 7 - [math]::Floor((15 - $y) / 2.3)); $dx = [math]::Abs($x - 7.5)
        $wave = [math]::Sin(($y + $x) * 0.9) * 1.2
        if ($dx -le $w + $wave) { Set-Px $c $x $y ($(if ($dx -le ($w + $wave) / 2.2) { 'i' } else { 'f' })) }
    } }
}
$c = New-Canvas '.'; Flame $c; Save $c @{ f = '#e8762a'; i = '#ffd35a' } 'blaze_burner_flame'
$c = New-Canvas '.'; Flame $c; Save $c @{ f = '#2fb9c4'; i = '#9cf3f0' } 'blaze_burner_flame_soul'
$c = New-Canvas '.'; Flame $c; Save $c @{ f = '#3d7fd8'; i = '#b9e6ff' } 'blaze_burner_flame_seething'

# --- fluids in the basin (client mod renderer) -----------------------------------------------------------------
$fluids = @{
    water     = @{ a = '#3f76e4'; b = '#5a8cf0' }
    lava      = @{ a = '#d0581a'; b = '#f2a22c' }
    milk      = @{ a = '#f2f2ee'; b = '#ffffff' }
    honey     = @{ a = '#e7a32a'; b = '#f6c454' }
    chocolate = @{ a = '#6b3e22'; b = '#86522f' }
    tea       = @{ a = '#a46b3c'; b = '#c18a55' }
    generic   = @{ a = '#9aa0a8'; b = '#b6bcc4' }
}
foreach ($f in $fluids.Keys) {
    $c = New-Canvas; for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        Set-Px $c $x $y ($(if ([math]::Sin($x * 0.8 + $y * 0.35) -gt 0.55) { 'b' } else { 'a' })) } }
    Save $c $fluids[$f] "basin_fluid_$f"
}

# =============================================================================================================
# Models
# =============================================================================================================

# A cube: origin / size in model pixels (x -8..8 mirrored as Bedrock, y up, z -8..8 north to south),
# per-face material instances (default 'side', up 'top', down 'bottom').
function Cube($o, $s, $mat = @{}, [string[]]$skip = @()) {
    $m = @{ north = 'side'; south = 'side'; east = 'side'; west = 'side'; up = 'top'; down = 'bottom' }
    foreach ($k in $mat.Keys) { $m[$k] = $mat[$k] }
    $wrap = { param($v, $size) $v = [double]$v; while ($v -lt 0) { $v += 16 }; while ($v + $size -gt 16.0001 -and $v -ge 16) { $v -= 16 }; [math]::Round($v, 3) }
    $x0 = $o[0]; $y0 = $o[1]; $z0 = $o[2]; $w = $s[0]; $h = $s[1]; $d = $s[2]
    $vTop = & $wrap (16 - ($y0 + $h)) $h
    $uv = [ordered]@{}
    $faces = [ordered]@{
        north = @((& $wrap (8 + $x0) $w), $vTop, $w, $h)
        south = @((& $wrap (8 + $x0) $w), $vTop, $w, $h)
        east  = @((& $wrap (8 + $z0) $d), $vTop, $d, $h)
        west  = @((& $wrap (8 + $z0) $d), $vTop, $d, $h)
        up    = @((& $wrap (8 + $x0) $w), (& $wrap (8 + $z0) $d), $w, $d)
        down  = @((& $wrap (8 + $x0) $w), (& $wrap (8 + $z0) $d), $w, $d)
    }
    foreach ($f in $faces.Keys) {
        if ($skip -contains $f) { continue }
        $v = $faces[$f]
        if ($v[2] -le 0 -or $v[3] -le 0) { continue }
        $uv[$f] = [ordered]@{ uv = @($v[0], $v[1]); uv_size = @($v[2], $v[3]); material_instance = $m[$f] }
    }
    [ordered]@{ origin = @($o); size = @($s); uv = $uv }
}
function Bone($name, $cubes, $pivot = @(0, 8, 0)) { [ordered]@{ name = $name; pivot = $pivot; cubes = @($cubes) } }
function Save-Geo($id, $bones, $file) {
    $doc = [ordered]@{
        format_version     = '1.16.0'
        'minecraft:geometry' = @([ordered]@{
                description = [ordered]@{ identifier = $id; texture_width = 16; texture_height = 16; visible_bounds_width = 4; visible_bounds_height = 4; visible_bounds_offset = @(0, 0.5, 0) }
                bones       = @($bones)
            })
    }
    $json = $doc | ConvertTo-Json -Depth 20
    [IO.File]::WriteAllText((Join-Path $models $file), $json + "`n")
}
New-Item -ItemType Directory -Force $models | Out-Null

# basin: floor and four walls (inner faces 'inner'), a spout per side
$in = 'inner'
$basin = @(
    (Bone 'body' @(
        (Cube @(-6, 0, -6) @(12, 2, 12) @{ up = $in }),
        (Cube @(-8, 2, -8) @(16, 14, 2) @{ south = $in }),
        (Cube @(-8, 2, 6) @(16, 14, 2) @{ north = $in }),
        (Cube @(-8, 2, -6) @(2, 14, 12) @{ east = $in }),
        (Cube @(6, 2, -6) @(2, 14, 12) @{ west = $in })
    )),
    (Bone 'spout_north' @((Cube @(-2, 9, -10) @(4, 3, 2) @{ up = $in }))),
    (Bone 'spout_south' @((Cube @(-2, 9, 8) @(4, 3, 2) @{ up = $in }))),
    (Bone 'spout_west' @((Cube @(8, 9, -2) @(2, 3, 4) @{ up = $in }))),
    (Bone 'spout_east' @((Cube @(-10, 9, -2) @(2, 3, 4) @{ up = $in })))
)
Save-Geo 'geometry.levitate.basin' $basin 'basin.geo.json'

# mechanical press (shaft along model x): casing, shaft, head
$press = @(
    (Bone 'casing' @(
        (Cube @(-8, 4, -8) @(16, 12, 16)),
        (Cube @(-8, 0, -8) @(2, 4, 2)), (Cube @(6, 0, -8) @(2, 4, 2)), (Cube @(-8, 0, 6) @(2, 4, 2)), (Cube @(6, 0, 6) @(2, 4, 2))
    )),
    (Bone 'shaft' @((Cube @(-8.5, 8, -2) @(17, 4, 4) @{ north = 'shaft'; south = 'shaft'; east = 'shaft'; west = 'shaft'; up = 'shaft'; down = 'shaft' }))),
    (Bone 'head' @(
        (Cube @(-1.5, 2, -1.5) @(3, 2, 3) @{ north = 'head'; south = 'head'; east = 'head'; west = 'head'; up = 'head'; down = 'head' }),
        (Cube @(-6, 0, -6) @(12, 2, 12) @{ north = 'head'; south = 'head'; east = 'head'; west = 'head'; up = 'head'; down = 'head' })
    ))
)
Save-Geo 'geometry.levitate.mechanical_press' $press 'mechanical_press.geo.json'
# the rotating shaft for the client mod, modelled around Y
Save-Geo 'geometry.levitate.mechanical_press_rotor' @(
    (Bone 'rotor' @((Cube @(-2, -0.5, -2) @(4, 17, 4) @{ north = 'shaft'; south = 'shaft'; east = 'shaft'; west = 'shaft'; up = 'shaft'; down = 'shaft' })))
) 'mechanical_press_rotor.geo.json'

# mechanical mixer: casing, cog (rotates around Y), whisk (hangs 7 px below the block at rest)
$cogFaces = @{ north = 'cog'; south = 'cog'; east = 'cog'; west = 'cog'; up = 'cog'; down = 'cog' }
$cogCubes = @(
    (Cube @(-6, 6, -6) @(12, 4, 12) $cogFaces),
    (Cube @(-8, 6, -2) @(16, 4, 4) $cogFaces),
    (Cube @(-2, 6, -8) @(4, 4, 16) $cogFaces)
)
$whiskFaces = @{ north = 'whisk'; south = 'whisk'; east = 'whisk'; west = 'whisk'; up = 'whisk'; down = 'whisk' }
$poleFaces = @{ north = 'pole'; south = 'pole'; east = 'pole'; west = 'pole'; up = 'pole'; down = 'pole' }
$whiskCubes = @(
    (Cube @(-1, -5, -1) @(2, 11, 2) $poleFaces),
    (Cube @(-4, -12, -4) @(8, 7, 8) $whiskFaces)
)
$mixer = @(
    (Bone 'casing' @((Cube @(-7, 10, -7) @(14, 6, 14)), (Cube @(-7, 0, -7) @(14, 6, 14)))),
    (Bone 'cog' $cogCubes),
    (Bone 'whisk' $whiskCubes)
)
Save-Geo 'geometry.levitate.mechanical_mixer' $mixer 'mechanical_mixer.geo.json'
Save-Geo 'geometry.levitate.mechanical_mixer_rotor' @((Bone 'rotor' $cogCubes)) 'mechanical_mixer_rotor.geo.json'
# the moving parts drawn by the client mod (one texture per bone)
Save-Geo 'geometry.levitate.mechanical_mixer_whisk' @((Bone 'pole' @($whiskCubes[0])), (Bone 'whisk' @($whiskCubes[1]))) 'mechanical_mixer_whisk.geo.json'

# blaze burner: base plate, cage, blaze head (face towards model north = the block's facing), flames
$headFaces = @{ north = 'face'; south = 'head'; east = 'head'; west = 'head'; up = 'head'; down = 'head' }
$cageFaces = @{ north = 'cage'; south = 'cage'; east = 'cage'; west = 'cage'; up = 'top'; down = 'bottom' }
$burner = @(
    (Bone 'base' @((Cube @(-8, 0, -8) @(16, 3, 16) @{ up = 'bottom' }))),
    (Bone 'cage' @(
        (Cube @(-8, 3, -8) @(16, 11, 0.01) $cageFaces @('up', 'down')),
        (Cube @(-8, 3, 8) @(16, 11, 0.01) $cageFaces @('up', 'down')),
        (Cube @(-8, 3, -8) @(0.01, 11, 16) $cageFaces @('up', 'down')),
        (Cube @(8, 3, -8) @(0.01, 11, 16) $cageFaces @('up', 'down'))
    )),
    (Bone 'head' @((Cube @(-4, 4, -4) @(8, 8, 8) $headFaces))),
    (Bone 'flame' @(
        [ordered]@{ origin = @(-6, 2, 0); size = @(12, 12, 0); uv = [ordered]@{ north = [ordered]@{ uv = @(2, 2); uv_size = @(12, 12); material_instance = 'flame' }; south = [ordered]@{ uv = @(2, 2); uv_size = @(12, 12); material_instance = 'flame' } } },
        [ordered]@{ origin = @(0, 2, -6); size = @(0, 12, 12); uv = [ordered]@{ east = [ordered]@{ uv = @(2, 2); uv_size = @(12, 12); material_instance = 'flame' }; west = [ordered]@{ uv = @(2, 2); uv_size = @(12, 12); material_instance = 'flame' } } }
    ))
)
Save-Geo 'geometry.levitate.blaze_burner' $burner 'blaze_burner.geo.json'
Save-Geo 'geometry.levitate.blaze_burner_head' @(
    (Bone 'head' @((Cube @(-4, 4, -4) @(8, 8, 8) $headFaces @('north')))),
    (Bone 'face' @([ordered]@{ origin = @(-4, 4, -4); size = @(8, 8, 0); uv = [ordered]@{ north = [ordered]@{ uv = @(4, 4); uv_size = @(8, 8); material_instance = 'face' } } }))
) 'blaze_burner_head.geo.json'

Write-Output 'basin textures and models written'
