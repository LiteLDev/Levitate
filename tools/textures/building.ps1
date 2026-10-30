# Textures of the building unit (U10): casings, connected glass, windows, copper roofing,
# scaffolding, ladders, bars, doors, trapdoors, girders, brackets, table cloths, placard and
# copycats. Original pixel art, drawn procedurally.
#
#   pwsh tools/textures/building.ps1            # writes assets/pack/rp/textures/blocks/building/
#   pwsh tools/textures/building.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
#
# Connected textures: "<name>_<mask>" where mask bits 1 = top, 2 = right, 4 = bottom, 8 = left
# edge continue into the neighbour (no frame drawn there). See tools/gen/building.ps1.
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$out = Join-Path $root 'assets/pack/rp/textures/blocks/building'
New-Item -ItemType Directory -Force $out | Out-Null

# ---- canvas helpers ---------------------------------------------------------------------------------
function New-Grid([int]$w = 16, [int]$h = 16, [char]$fill = '.') { , (0..($h - 1) | ForEach-Object { , ([char[]]((New-Object string $fill, $w))) }) }
function Px($g, [int]$x, [int]$y, [char]$k) { if ($y -ge 0 -and $y -lt $g.Count -and $x -ge 0 -and $x -lt $g[0].Length) { $g[$y][$x] = $k } }
function Rect($g, [int]$x0, [int]$y0, [int]$x1, [int]$y1, [char]$k) { for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { Px $g $x $y $k } } }
function Save($g, $pal, [string]$name) {
    $rows = [string[]]@($g | ForEach-Object { -join $_ })
    Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $out "$name.png")
    if ($Preview) { Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $root "tools/textures/preview/building_$name.png") -Scale 8 }
}
function Pal($base, $extra) { $h = @{}; foreach ($k in $base.Keys) { $h[$k] = $base[$k] }; foreach ($k in $extra.Keys) { $h[$k] = $extra[$k] }; $h }
# deterministic noise in 0..1
function Noise([int]$x, [int]$y, [int]$seed) { $n = ($x * 374761393 + $y * 668265263 + $seed * 2147483647) -band 0x7fffffff; $n = (($n -bxor ($n -shr 13)) * 1274126177) -band 0x7fffffff; return ($n % 1000) / 1000.0 }

# A frame of width $w along the edges that do not connect. H = lit outer row, S = shadow outer
# row, F = frame body. Bolts (n) sit in the corners where both edges are framed.
function Ct-Frame($g, [int]$mask, [int]$w = 2, [switch]$Bolts) {
    $top = ($mask -band 1) -eq 0; $right = ($mask -band 2) -eq 0; $bottom = ($mask -band 4) -eq 0; $left = ($mask -band 8) -eq 0
    $n = $g.Count - 1
    if ($top) { Rect $g 0 0 $n ($w - 1) 'F'; Rect $g 0 0 $n 0 'H' }
    if ($bottom) { Rect $g 0 ($n - $w + 1) $n $n 'F'; Rect $g 0 $n $n $n 'S' }
    if ($left) { Rect $g 0 0 ($w - 1) $n 'F'; Rect $g 0 0 0 $n 'H' }
    if ($right) { Rect $g ($n - $w + 1) 0 $n $n 'F'; Rect $g $n 0 $n $n 'S' }
    # inner shadow line under a lit frame edge
    if ($top) { for ($x = 0; $x -le $n; $x++) { if ($g[$w][$x] -notin 'F', 'H', 'S') { $g[$w][$x] = 'd' } } }
    if ($left) { for ($y = 0; $y -le $n; $y++) { if ($g[$y][$w] -notin 'F', 'H', 'S') { $g[$y][$w] = 'd' } } }
    if ($Bolts) {
        if ($top -and $left) { Px $g ($w) ($w) 'n' }
        if ($top -and $right) { Px $g ($n - $w) ($w) 'n' }
        if ($bottom -and $left) { Px $g ($w) ($n - $w) 'n' }
        if ($bottom -and $right) { Px $g ($n - $w) ($n - $w) 'n' }
    }
}

# ---- interiors ---------------------------------------------------------------------------------------
function Planks($g, [int]$seed) {
    for ($y = 0; $y -lt 16; $y++) {
        for ($x = 0; $x -lt 16; $x++) {
            $row = [Math]::Floor($y / 4)
            $k = if ($y % 4 -eq 3) { 'd' } elseif (((($x + $row * 7) % 16) -eq 0)) { 'd' } elseif ((Noise $x $y $seed) -gt 0.86) { 'l' } elseif ((Noise $x $y ($seed + 9)) -gt 0.9) { 'k' } else { 'p' }
            $g[$y][$x] = [char]$k
        }
    }
}
function Plates($g, [int]$seed) {
    for ($y = 0; $y -lt 16; $y++) {
        for ($x = 0; $x -lt 16; $x++) {
            $k = if ($x % 8 -eq 7 -or $y % 8 -eq 7) { 'd' } elseif ($x % 8 -eq 0 -or $y % 8 -eq 0) { 'l' } elseif ((Noise $x $y $seed) -gt 0.9) { 'k' } else { 'p' }
            $g[$y][$x] = [char]$k
        }
    }
    foreach ($p in @(@(2, 2), @(5, 5), @(10, 2), @(13, 5), @(2, 10), @(5, 13), @(10, 10), @(13, 13))) { Px $g $p[0] $p[1] 'n' }
}
function Diagonals($g, [int]$seed) {
    for ($y = 0; $y -lt 16; $y++) {
        for ($x = 0; $x -lt 16; $x++) {
            $k = if ((($x + $y) % 8) -eq 0) { 'l' } elseif ((($x + $y) % 8) -eq 1) { 'd' } elseif ((Noise $x $y $seed) -gt 0.92) { 'k' } else { 'p' }
            $g[$y][$x] = [char]$k
        }
    }
}
function Sparkle($g, [int]$seed) {
    for ($y = 0; $y -lt 16; $y++) {
        for ($x = 0; $x -lt 16; $x++) {
            $r = Noise $x $y $seed
            $k = if ($r -gt 0.95) { 'k' } elseif ($r -gt 0.75) { 'l' } elseif ((($x * 3 + $y * 5) % 11) -eq 0) { 'd' } else { 'p' }
            $g[$y][$x] = [char]$k
        }
    }
}

# ---- casings -----------------------------------------------------------------------------------------
$casings = @(
    @{ id = 'andesite_casing'; fill = 'Planks'; pal = @{ F = '#a5a99c'; H = '#c9ccc1'; S = '#6f7268'; n = '#56584f'; p = '#9c7a4f'; l = '#b38f5f'; d = '#6e5233'; k = '#86683f' } }
    @{ id = 'brass_casing'; fill = 'Planks'; pal = @{ F = '#d9a646'; H = '#f4d27c'; S = '#9a6a22'; n = '#7a4f17'; p = '#7a5a3a'; l = '#8f6c46'; d = '#4f3823'; k = '#694c30' } }
    @{ id = 'copper_casing'; fill = 'Planks'; pal = @{ F = '#c8734c'; H = '#eba179'; S = '#8a4a30'; n = '#5e3220'; p = '#8d6a46'; l = '#a37d55'; d = '#5e452b'; k = '#76583a' } }
    @{ id = 'shadow_steel_casing'; fill = 'Diagonals'; pal = @{ F = '#3a3445'; H = '#5d5470'; S = '#1d1924'; n = '#7b67a8'; p = '#26222e'; l = '#3c3549'; d = '#18151d'; k = '#4b3f63' } }
    @{ id = 'refined_radiance_casing'; fill = 'Sparkle'; pal = @{ F = '#e6e1cf'; H = '#ffffff'; S = '#b8b29a'; n = '#fff5c2'; p = '#f2efe4'; l = '#fffdf2'; d = '#d7d1bb'; k = '#fff7b0' } }
    @{ id = 'railway_casing'; fill = 'Plates'; pal = @{ F = '#47444f'; H = '#6b6878'; S = '#25232b'; n = '#8c8a96'; p = '#3a3841'; l = '#4f4c58'; d = '#2a2830'; k = '#45424d' } }
)
$seed = 1
foreach ($c in $casings) {
    for ($mask = 0; $mask -lt 16; $mask++) {
        $g = New-Grid
        & $c.fill $g $seed
        Ct-Frame $g $mask 2 -Bolts
        Save $g $c.pal "$($c.id)_$mask"
    }
    $seed += 3
}

# ---- connected glass -------------------------------------------------------------------------------------
$glassPal = @{ F = '#c3ccd0'; H = '#eef4f6'; S = '#8b979c'; d = '#a9c2cb'; n = '#6f7c82'; g = '#dceff5' }
function Glass-Glints($g) { foreach ($p in @(@(4, 3), @(3, 4), @(5, 4), @(11, 10), @(12, 11), @(10, 11))) { if ($g[$p[1]][$p[0]] -eq '.') { Px $g $p[0] $p[1] 'g' } } }
function Glass-Frame($g, [int]$mask) {
    $n = 15
    if (($mask -band 1) -eq 0) { Rect $g 0 0 $n 0 'H' }
    if (($mask -band 4) -eq 0) { Rect $g 0 $n $n $n 'S' }
    if (($mask -band 8) -eq 0) { Rect $g 0 0 0 $n 'H' }
    if (($mask -band 2) -eq 0) { Rect $g $n 0 $n $n 'S' }
}
foreach ($mask in 0..15) {
    $g = New-Grid; Glass-Frame $g $mask; Glass-Glints $g; Save $g $glassPal "framed_glass_$mask"
}
$g = New-Grid; Glass-Frame $g 0; Glass-Glints $g; Save $g $glassPal 'framed_glass_pane'
foreach ($mask in 0, 2, 8, 10) {
    $g = New-Grid; Glass-Frame $g ($mask -bor 0); Rect $g 0 7 15 8 'F'; Glass-Glints $g
    if (($mask -band 8) -ne 0) { Px $g 0 0 'H'; Px $g 0 15 'S' }
    Save $g $glassPal "horizontal_framed_glass_$mask"
}
$g = New-Grid; Glass-Frame $g 0; Rect $g 0 7 15 8 'F'; Glass-Glints $g; Save $g $glassPal 'horizontal_framed_glass_end'
$g = New-Grid; Glass-Frame $g 0; Rect $g 0 7 15 8 'F'; Glass-Glints $g; Save $g $glassPal 'horizontal_framed_glass_pane'
foreach ($mask in 0, 1, 4, 5) {
    $g = New-Grid; Glass-Frame $g $mask; Rect $g 7 0 8 15 'F'; Glass-Glints $g; Save $g $glassPal "vertical_framed_glass_$mask"
}
$g = New-Grid; Glass-Frame $g 0; Rect $g 7 0 8 15 'F'; Glass-Glints $g; Save $g $glassPal 'vertical_framed_glass_end'
$g = New-Grid; Glass-Frame $g 0; Rect $g 7 0 8 15 'F'; Glass-Glints $g; Save $g $glassPal 'vertical_framed_glass_pane'
foreach ($name in 'tiled_glass', 'tiled_glass_pane') {
    $g = New-Grid; Glass-Frame $g 0; Rect $g 0 7 15 7 'F'; Rect $g 7 0 7 15 'F'; Rect $g 0 8 15 8 'S'; Rect $g 8 0 8 15 'S'; Px $g 7 7 'n'; Glass-Glints $g
    Save $g $glassPal $name
}

# ---- windows ---------------------------------------------------------------------------------------------------
$woodPal = @{
    oak = @('#b8945f', '#d4b07a', '#7f6038'); spruce = @('#7a5a3a', '#97754f', '#4d3623'); birch = @('#d7c88f', '#efe3b2', '#a8986a')
    jungle = @('#b0805a', '#c99a72', '#7a5236'); acacia = @('#b0623a', '#cc7d52', '#7a3f22'); dark_oak = @('#4f3420', '#6a4a2f', '#2e1d10')
    mangrove = @('#7d352f', '#9a4a43', '#53201b'); crimson = @('#6e3550', '#8a4a68', '#46203a'); warped = @('#3a8a84', '#55a8a0', '#225d58')
    cherry = @('#e2b1aa', '#f3cbc5', '#b8857f'); bamboo = @('#d3c05b', '#e9d878', '#a08f35')
    ornate_iron = @('#5c5d60', '#8d8f93', '#36373a'); industrial_iron = @('#6b6d70', '#9a9da1', '#424447'); weathered_iron = @('#7b5c47', '#a07a5d', '#4f3a2c')
}
foreach ($w in $woodPal.Keys) {
    $c = $woodPal[$w]
    $pal = @{ F = $c[0]; H = $c[1]; S = $c[2]; d = $c[2]; n = $c[2]; g = '#d6ecf2'; m = $c[0]; q = $c[2] }
    $isIron = $w -like '*iron'
    foreach ($mask in 0..15) {
        $g = New-Grid
        Ct-Frame $g $mask 2
        # mullions: a cross for wood, a lattice for ornate iron, a grid for industrial windows
        if ($w -eq 'ornate_iron') {
            for ($i = 2; $i -lt 14; $i++) { foreach ($k in @($i, (15 - $i))) { if ($g[$i][$k] -eq '.') { $g[$i][$k] = 'm' } } }
            foreach ($p in @(@(7, 3), @(8, 3), @(7, 12), @(8, 12), @(3, 7), @(3, 8), @(12, 7), @(12, 8))) { Px $g $p[0] $p[1] 'm' }
        } elseif ($isIron) {
            foreach ($i in 5, 10) { for ($t = 0; $t -lt 16; $t++) { if ($g[$t][$i] -eq '.') { $g[$t][$i] = 'm' }; if ($g[$i][$t] -eq '.') { $g[$i][$t] = 'm' } } }
        } else {
            for ($t = 0; $t -lt 16; $t++) { if ($g[$t][7] -eq '.') { $g[$t][7] = 'm' }; if ($g[$t][8] -eq '.') { $g[$t][8] = 'q' } }
            for ($t = 2; $t -lt 14; $t++) { if ($g[7][$t] -eq '.') { $g[7][$t] = 'm' }; if ($g[8][$t] -eq '.') { $g[8][$t] = 'q' } }
        }
        Glass-Glints $g
        Save $g $pal "${w}_window_$mask"
        if ($mask -eq 0) { Save $g $pal "${w}_window_pane"; Save $g $pal "${w}_window_end" }
    }
}

# ---- copper shingles and tiles -----------------------------------------------------------------------------------
$copperStages = [ordered]@{
    fresh = @('#c06a4b', '#e29270', '#8a4630', '#d07c5a'); exposed = @('#a7806a', '#c49e86', '#7a5a4a', '#b28d72')
    weathered = @('#6d9b7f', '#8dbb9d', '#4a7058', '#7aa98a'); oxidized = @('#52a48e', '#76c4ad', '#357663', '#62b39c')
}
foreach ($s in $copperStages.Keys) {
    $c = $copperStages[$s]
    $pal = @{ p = $c[0]; l = $c[1]; d = $c[2]; k = $c[3] }
    # shingles: rows of rounded scales, every row shifted by half a scale
    $g = New-Grid
    for ($y = 0; $y -lt 16; $y++) {
        for ($x = 0; $x -lt 16; $x++) {
            $row = [Math]::Floor($y / 4); $ly = $y % 4; $lx = ($x + ($row % 2) * 4) % 8
            $k = if ($ly -eq 3) { 'd' } elseif ($lx -eq 0 -and $ly -ge 1) { 'd' } elseif ($ly -eq 0 -and ($lx -eq 3 -or $lx -eq 4)) { 'l' } elseif ($ly -eq 0) { 'k' } else { 'p' }
            if ($k -eq 'p' -and (Noise $x $y 77) -gt 0.88) { $k = 'k' }
            $g[$y][$x] = [char]$k
        }
    }
    Save $g $pal "copper_shingles_$s"
    # tiles: four bevelled squares
    $g = New-Grid
    for ($y = 0; $y -lt 16; $y++) {
        for ($x = 0; $x -lt 16; $x++) {
            $lx = $x % 8; $ly = $y % 8
            $k = if ($lx -eq 7 -or $ly -eq 7) { 'd' } elseif ($lx -eq 0 -or $ly -eq 0) { 'l' } elseif ($lx -eq 6 -or $ly -eq 6) { 'k' } elseif ((Noise $x $y 91) -gt 0.9) { 'k' } else { 'p' }
            $g[$y][$x] = [char]$k
        }
    }
    Save $g $pal "copper_tiles_$s"
}

# ---- metals: scaffolding, ladders, bars, table covers -----------------------------------------------------------
$metalPal = @{
    andesite = @{ F = '#a5a99c'; H = '#c9ccc1'; S = '#6f7268'; n = '#56584f'; p = '#8f9386'; d = '#7d8175' }
    brass    = @{ F = '#d9a646'; H = '#f4d27c'; S = '#9a6a22'; n = '#7a4f17'; p = '#c08d3e'; d = '#a87a30' }
    copper   = @{ F = '#c8734c'; H = '#eba179'; S = '#8a4a30'; n = '#5e3220'; p = '#b5694d'; d = '#985a3f' }
}
foreach ($m in $metalPal.Keys) {
    $pal = $metalPal[$m]
    # scaffolding: a solid 2-px frame (the legs use it) around a see-through grate
    $g = New-Grid
    Ct-Frame $g 0 2 -Bolts
    for ($y = 2; $y -lt 14; $y++) { for ($x = 2; $x -lt 14; $x++) { if ((($x + $y) % 4) -eq 0 -or (($x - $y + 16) % 4) -eq 0) { $g[$y][$x] = 'p' } } }
    Save $g $pal "${m}_scaffolding"
    # ladder: rails at x 1..2 / 13..14, rungs every 4 px
    $g = New-Grid
    Rect $g 1 0 2 15 'F'; Rect $g 1 0 1 15 'H'; Rect $g 13 0 14 15 'F'; Rect $g 14 0 14 15 'S'
    foreach ($y in 1, 5, 9, 13) { Rect $g 3 $y 12 $y 'p'; Px $g 3 $y 'n'; Px $g 12 $y 'n' }
    Save $g $pal "${m}_ladder"
    # bars: vertical bars with top and bottom rails; the post uses columns 7..8
    $g = New-Grid
    foreach ($x in 1, 4, 7, 8, 11, 14) { Rect $g $x 0 $x 15 'F' }
    Rect $g 0 1 15 2 'p'; Rect $g 0 13 15 14 'p'; Rect $g 0 1 15 1 'H'; Rect $g 0 14 15 14 'S'
    Save $g $pal "${m}_bars"
    # table cover: a metal sheet with a rim
    $g = New-Grid
    Rect $g 0 0 15 15 'p'; Ct-Frame $g 0 1
    for ($y = 3; $y -lt 13; $y += 3) { Rect $g 3 $y 12 $y 'S' }
    Save $g $pal "${m}_table_cloth"
}

# ---- table cloths -------------------------------------------------------------------------------------------------
$dye = [ordered]@{
    white = '#e9ecec'; orange = '#f07613'; magenta = '#bd44b3'; light_blue = '#3aafd9'; yellow = '#f8c527'; lime = '#70b919'
    pink = '#ed8dac'; gray = '#3e4447'; light_gray = '#8e8e86'; cyan = '#158991'; purple = '#792aac'; blue = '#35399d'
    brown = '#724728'; green = '#546d1b'; red = '#a12722'; black = '#141519'
}
foreach ($c in $dye.Keys) {
    $pal = @{ p = $dye[$c]; l = (Shade-Color $dye[$c] 1.25); d = (Shade-Color $dye[$c] 0.72); k = (Shade-Color $dye[$c] 0.86) }
    $g = New-Grid
    for ($y = 0; $y -lt 16; $y++) {
        for ($x = 0; $x -lt 16; $x++) {
            $k = if ($x -eq 0 -or $y -eq 0 -or $x -eq 15 -or $y -eq 15) { 'd' } elseif ($x -eq 2 -or $y -eq 2 -or $x -eq 13 -or $y -eq 13) { 'l' } elseif ((($x + $y) % 2) -eq 0) { 'p' } else { 'k' }
            $g[$y][$x] = [char]$k
        }
    }
    Save $g $pal "${c}_table_cloth"
}

# ---- doors and trapdoors (16 x 32: upper half on top) ---------------------------------------------------------------
$doorPal = @{
    andesite_door     = @{ F = '#a5a99c'; H = '#c9ccc1'; S = '#6f7268'; n = '#56584f'; p = '#8f9386'; d = '#6f7268'; g = '#cfe6ee'; glass = $true }
    brass_door        = @{ F = '#d9a646'; H = '#f4d27c'; S = '#9a6a22'; n = '#7a4f17'; p = '#c08d3e'; d = '#9a6a22'; g = '#cfe6ee'; glass = $true }
    copper_door       = @{ F = '#c8734c'; H = '#eba179'; S = '#8a4a30'; n = '#5e3220'; p = '#b5694d'; d = '#8a4a30'; g = '#cfe6ee'; glass = $true }
    train_door        = @{ F = '#47444f'; H = '#6b6878'; S = '#25232b'; n = '#8c8a96'; p = '#3a3841'; d = '#2a2830'; g = '#cfe6ee'; glass = $true }
    framed_glass_door = @{ F = '#c3ccd0'; H = '#eef4f6'; S = '#8b979c'; n = '#6f7c82'; p = '#c3ccd0'; d = '#8b979c'; g = '#dceff5'; glass = $true }
}
foreach ($id in $doorPal.Keys) {
    $pal = $doorPal[$id]; $p = @{}; foreach ($k in $pal.Keys) { if ($k -ne 'glass') { $p[$k] = $pal[$k] } }
    $g = New-Grid 16 32
    if ($id -eq 'framed_glass_door') {
        Rect $g 0 0 15 31 '.'
        Rect $g 0 0 1 31 'F'; Rect $g 14 0 15 31 'F'; Rect $g 0 0 15 1 'F'; Rect $g 0 30 15 31 'F'; Rect $g 0 15 15 16 'F'
        Rect $g 0 0 0 31 'H'; Rect $g 15 0 15 31 'S'
        foreach ($q in @(@(4, 4), @(5, 5), @(10, 20), @(11, 21))) { Px $g $q[0] $q[1] 'g' }
    } else {
        Rect $g 0 0 15 31 'p'
        Rect $g 0 0 1 31 'F'; Rect $g 14 0 15 31 'F'; Rect $g 0 0 15 1 'F'; Rect $g 0 30 15 31 'F'; Rect $g 0 15 15 16 'F'
        Rect $g 0 0 0 31 'H'; Rect $g 15 0 15 31 'S'
        Rect $g 4 4 11 11 '.'; Rect $g 3 3 12 3 'd'; Rect $g 3 3 3 12 'd'; Rect $g 3 12 12 12 'H'; Rect $g 12 3 12 12 'H'
        foreach ($q in @(@(5, 5), @(6, 6), @(9, 9))) { Px $g $q[0] $q[1] 'g' }
        foreach ($y in 20, 24) { Rect $g 3 $y 12 $y 'd' }
        Px $g 12 23 'n'; Px $g 3 23 'n'
    }
    Save $g $p $id
}
foreach ($id in 'train_trapdoor', 'framed_glass_trapdoor') {
    $pal = if ($id -eq 'train_trapdoor') { $doorPal['train_door'] } else { $doorPal['framed_glass_door'] }
    $p = @{}; foreach ($k in $pal.Keys) { if ($k -ne 'glass') { $p[$k] = $pal[$k] } }
    $g = New-Grid
    if ($id -eq 'train_trapdoor') { Plates $g 5; $p.l = '#4f4c58'; $p.k = '#45424d' }
    Ct-Frame $g 0 2 -Bolts
    if ($id -eq 'framed_glass_trapdoor') { Rect $g 7 2 8 13 'F'; Glass-Glints $g } else { Rect $g 4 6 11 9 '.'; Rect $g 4 6 11 6 'd' }
    Save $g $p $id
}

# ---- girder, brackets, placard, copycat ------------------------------------------------------------------------------
$steel = @{ F = '#4f525a'; H = '#7c808a'; S = '#2c2e33'; n = '#1f2024'; p = '#5d6068'; d = '#3a3c42'; l = '#6c7079'; k = '#55585f' }
$g = New-Grid; Plates $g 21; Ct-Frame $g 0 1; foreach ($q in @(@(4, 7), @(11, 7), @(7, 4), @(7, 11))) { Rect $g $q[0] $q[1] ($q[0] + 1) ($q[1] + 1) 'n' }
Save $g $steel 'metal_girder'
$g = New-Grid
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { $g[$y][$x] = [char]$(if ((($y + [Math]::Floor($x / 4)) % 4) -eq 0) { 'd' } elseif ($x % 4 -eq 0) { 'l' } else { 'p' }) } }
Save $g @{ p = '#9a9d92'; l = '#c0c3b7'; d = '#6c6f66' } 'girder_shaft'
$g = New-Grid; Rect $g 0 0 15 15 'p'; Ct-Frame $g 0 1 -Bolts; Save $g $steel 'metal_bracket'
$g = New-Grid; Planks $g 31; Ct-Frame $g 0 1 -Bolts; Save $g @{ F = '#6e5233'; H = '#a37d55'; S = '#4a3420'; n = '#3a2a1a'; p = '#9c7a4f'; l = '#b38f5f'; d = '#6e5233'; k = '#86683f' } 'wooden_bracket'
# bracket rings (Create's clamp around the shaft): a band with a hole the size of the shaft (6..10)
foreach ($b in @(@('metal_bracket', $steel), @('wooden_bracket', @{ F = '#6e5233'; H = '#a37d55'; S = '#4a3420'; n = '#3a2a1a'; p = '#9c7a4f'; l = '#b38f5f'; d = '#6e5233'; k = '#86683f' }))) {
    $g = New-Grid; Rect $g 0 0 15 15 'F'; Rect $g 0 0 15 0 'H'; Rect $g 0 15 15 15 'S'; Rect $g 5 5 10 10 'S'; Rect $g 6 6 9 9 '.'
    Save $g $b[1] "$($b[0])_ring"
}
$brass = @{ F = '#d9a646'; H = '#f4d27c'; S = '#9a6a22'; n = '#7a4f17'; p = '#3b2d22'; l = '#4a3a2c'; d = '#2a2018'; k = '#ffd76a' }
$g = New-Grid; Rect $g 0 0 15 15 'p'; Ct-Frame $g 0 2 -Bolts; Save $g $brass 'placard'
$g = New-Grid; Rect $g 0 0 15 15 'p'; Ct-Frame $g 0 2 -Bolts; Rect $g 2 2 13 2 'k'; Rect $g 2 13 13 13 'k'; Rect $g 2 2 2 13 'k'; Rect $g 13 2 13 13 'k'; Save $g $brass 'placard_powered'
$zinc = @{ F = '#9fa4a0'; H = '#c7ccc8'; S = '#6d726e'; n = '#525653'; p = '#8a8f8b'; d = '#717672'; l = '#a9aeaa'; k = '#7d827e' }
$g = New-Grid; Diagonals $g 41; Ct-Frame $g 0 2 -Bolts; Save $g $zinc 'copycat_base'

# ---- check that every texture the generator references exists ------------------------------------------------------
$terrain = Get-Content (Join-Path $root 'assets/pack/rp/terrain_texture.d/building.json') -Raw | ConvertFrom-Json -AsHashtable
$missing = @($terrain.Values | ForEach-Object { $_.textures } | Where-Object { -not (Test-Path (Join-Path $root "assets/pack/rp/$_.png")) })
if ($missing.Count -gt 0) { throw "missing textures: $($missing -join ', ')" }
Write-Host "building textures: $((Get-ChildItem $out -Filter *.png).Count) files"
