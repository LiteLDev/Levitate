# Textures of the packages unit (U25): packager, re-packager, chain conveyor, frogport, postboxes,
# cardboard packages (all box styles) and the package filter. Original pixel art.
#
#   pwsh tools/textures/packages.ps1            # writes assets/pack/rp/textures/{blocks,items}/
#   pwsh tools/textures/packages.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$blocks = Join-Path $root 'assets/pack/rp/textures/blocks'
$items = Join-Path $root 'assets/pack/rp/textures/items'

# --- canvas helpers (n x n char grids) ------------------------------------------------------------
function New-Canvas([int]$n = 16, [char]$fill = '.') { , (0..($n - 1) | ForEach-Object { , ([char[]]((New-Object string $fill, $n))) }) }
function Set-Px($c, $x, $y, [char]$k) { if ($y -ge 0 -and $y -lt $c.Count -and $x -ge 0 -and $x -lt $c[0].Count) { $c[$y][$x] = $k } }
function Fill($c, $x0, $y0, $x1, $y1, [char]$k) { for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y $k } } }
function Frame($c, $x0, $y0, $x1, $y1, [char]$light, [char]$dark) {
    Fill $c $x0 $y0 $x1 $y0 $light; Fill $c $x0 $y0 $x0 $y1 $light
    Fill $c $x0 $y1 $x1 $y1 $dark; Fill $c $x1 $y0 $x1 $y1 $dark
}
function Noise($c, $x0, $y0, $x1, $y1, [char]$k, [int]$seed, [int]$mod = 7) {
    for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { if ((($x * 7 + $y * 13 + $seed) % $mod) -eq 0) { Set-Px $c $x $y $k } } }
}
function Save($c, $pal, $path) {
    $rows = , ($c | ForEach-Object { -join $_ })
    Save-PixelArt -Rows $rows[0] -Palette $pal -Path $path
    if ($Preview) { Save-PixelArt -Rows $rows[0] -Palette $pal -Path (Join-Path $root "tools/textures/preview/$(Split-Path -Leaf $path)") -Scale 8 }
}

# --- packager / re-packager: a cardboard-filled iron frame --------------------------------------------
$cardboard = @{ p = '#b88a52'; q = '#a27744'; t = '#d8b47a'; f = '#8d9095'; h = '#c3c6ca'; s = '#5d6066'; k = '#2b2523'; r = '#e0d9c8'; y = '#c9a13b'; z = '#8c6d1f'; a = '#6b4e2b' }
function Packager-Side($c, [char]$frameLight, [char]$frameDark) {
    Fill $c 0 0 15 15 'p'; Noise $c 1 1 14 14 'q' 3 5
    Fill $c 7 1 8 14 't'                         # tape strip
    Fill $c 0 0 15 1 'f'; Fill $c 0 14 15 15 'f'; Fill $c 0 0 1 15 'f'; Fill $c 14 0 15 15 'f'
    Frame $c 0 0 15 15 $frameLight $frameDark
    foreach ($p in @(@(1, 1), @(14, 1), @(1, 14), @(14, 14))) { Set-Px $c $p[0] $p[1] 'k' }
}
function Packager-Front($c, [char]$frameLight, [char]$frameDark) {
    Packager-Side $c $frameLight $frameDark
    Fill $c 2 4 13 11 'k'                        # the tray opening
    Fill $c 2 10 13 11 'a'; Fill $c 3 11 12 11 'q'
    Frame $c 2 4 13 11 's' 'h'
}
function Packager-Back($c, [char]$frameLight, [char]$frameDark) {
    Packager-Side $c $frameLight $frameDark
    Fill $c 4 4 11 11 's'; Frame $c 4 4 11 11 'h' 'k'
    foreach ($p in @(@(6, 6), @(9, 6), @(6, 9), @(9, 9))) { Fill $c $p[0] $p[1] ($p[0] + 1) ($p[1] + 1) 'k' }
}
foreach ($set in @(@{ name = 'packager'; l = 'h'; d = 's' }, @{ name = 'repackager'; l = 'y'; d = 'z' })) {
    $c = New-Canvas; Packager-Side $c $set.l $set.d
    if ($set.name -eq 'repackager') { foreach ($i in 3..6) { Set-Px $c (2 + $i) (3 + $i) 'r'; Set-Px $c (13 - $i) (3 + $i) 'r' } }
    Save $c $cardboard "$blocks/$($set.name)_side.png"
    $c = New-Canvas; Packager-Front $c $set.l $set.d; Save $c $cardboard "$blocks/$($set.name)_front.png"
    $c = New-Canvas; Packager-Back $c $set.l $set.d; Save $c $cardboard "$blocks/$($set.name)_back.png"
}

# --- 32x32 model textures: four 16x16 zones (a top-left, b top-right, c bottom-left, d bottom-right) ---
function Zone($c, [int]$zx, [int]$zy, [scriptblock]$draw) { & $draw ($zx * 16) ($zy * 16) }

# chain conveyor: andesite casing, the chain wheel, dark interior, shaft
$andesite = @{ a = '#a3a69e'; b = '#878a82'; e = '#c4c7bf'; k = '#4b4d48'; w = '#5c5e63'; x = '#797b80'; c = '#3a3b3e'; m = '#9c7a4a'; n = '#7a5c34'; o = '#2b2b2d' }
$c = New-Canvas 32
Zone $c 0 0 { param($x, $y) Fill $c $x $y ($x + 15) ($y + 15) 'a'; Noise $c $x $y ($x + 15) ($y + 15) 'b' 1 6; Frame $c $x $y ($x + 15) ($y + 15) 'e' 'k' }
Zone $c 1 0 { param($x, $y) Fill $c $x $y ($x + 15) ($y + 15) 'w'; for ($i = 0; $i -lt 16; $i += 3) { Fill $c ($x + $i) $y ($x + $i) ($y + 15) 'x' }; Fill $c $x ($y + 7) ($x + 15) ($y + 8) 'c'; Frame $c $x $y ($x + 15) ($y + 15) 'x' 'o' }
Zone $c 0 1 { param($x, $y) Fill $c $x $y ($x + 15) ($y + 15) 'c'; Noise $c $x $y ($x + 15) ($y + 15) 'o' 2 5 }
Zone $c 1 1 { param($x, $y) Fill $c $x $y ($x + 15) ($y + 15) 'm'; for ($i = 0; $i -lt 16; $i += 4) { Fill $c $x ($y + $i) ($x + 15) ($y + $i) 'n' } }
Save $c $andesite "$blocks/chain_conveyor.png"

# frogport: green skin with spots, eyes, andesite base, brass goggles
$frog = @{ g = '#6fa24a'; h = '#8cc35c'; d = '#4d7a31'; s = '#3b5f24'; w = '#f2f2e6'; k = '#1d1d1d'; a = '#9a9d96'; b = '#7d8079'; y = '#c9a13b'; z = '#8c6d1f'; l = '#9fd3e6' }
$c = New-Canvas 32
Zone $c 0 0 { param($x, $y) Fill $c $x $y ($x + 15) ($y + 15) 'g'; Noise $c $x $y ($x + 15) ($y + 15) 'd' 4 7; Noise $c $x $y ($x + 15) ($y + 15) 'h' 2 11; Fill $c $x ($y + 14) ($x + 15) ($y + 15) 's' }
Zone $c 1 0 { param($x, $y) Fill $c $x $y ($x + 15) ($y + 15) 'w'; Fill $c ($x + 1) ($y + 1) ($x + 1) ($y + 1) 'k'; Fill $c ($x + 5) ($y + 5) ($x + 10) ($y + 10) 'k'; Frame $c $x $y ($x + 15) ($y + 15) 'g' 'd' }
Zone $c 0 1 { param($x, $y) Fill $c $x $y ($x + 15) ($y + 15) 'a'; Noise $c $x $y ($x + 15) ($y + 15) 'b' 3 5 }
Zone $c 1 1 { param($x, $y) Fill $c $x $y ($x + 15) ($y + 15) 'y'; Fill $c ($x + 3) ($y + 3) ($x + 12) ($y + 12) 'l'; Frame $c $x $y ($x + 15) ($y + 15) 'y' 'z' }
Save $c $frog "$blocks/package_frogport.png"

# postboxes: dyed body, darker lid, wooden post, flag
$dyes = [ordered]@{
    white = '#e8e8e2'; orange = '#e07c22'; magenta = '#bd47b5'; light_blue = '#4fb4dc'; yellow = '#f2c93a'; lime = '#80c632'; pink = '#ee8fae'; gray = '#4a4f53'
    light_gray = '#9c9c96'; cyan = '#16898f'; purple = '#7a2fb0'; blue = '#35399d'; brown = '#7a4e2c'; green = '#5a7524'; red = '#a8302a'; black = '#1f1f24'
}
foreach ($name in $dyes.Keys) {
    $base = $dyes[$name]
    $flag = if ($name -eq 'red' -or $name -eq 'orange' -or $name -eq 'pink' -or $name -eq 'magenta') { '#f2d13a' } else { '#d4362c' }
    $pal = @{ c = $base; l = (Shade-Color $base 1.25); d = (Shade-Color $base 0.72); e = (Shade-Color $base 0.55); w = '#8a6338'; v = '#6b4a28'; f = $flag; g = (Shade-Color $flag 0.7); k = '#202020'; s = '#b5b8bd' }
    $c = New-Canvas 32
    Zone $c 0 0 { param($x, $y) Fill $c $x $y ($x + 15) ($y + 15) 'c'; Noise $c $x $y ($x + 15) ($y + 15) 'd' 5 9; Frame $c $x $y ($x + 15) ($y + 15) 'l' 'e'; Fill $c ($x + 4) ($y + 4) ($x + 11) ($y + 5) 'k'; Fill $c ($x + 4) ($y + 6) ($x + 11) ($y + 6) 's' }
    Zone $c 1 0 { param($x, $y) Fill $c $x $y ($x + 15) ($y + 15) 'd'; Frame $c $x $y ($x + 15) ($y + 15) 'c' 'e' }
    Zone $c 0 1 { param($x, $y) Fill $c $x $y ($x + 15) ($y + 15) 'w'; for ($i = 0; $i -lt 16; $i += 4) { Fill $c ($x + $i) $y ($x + $i) ($y + 15) 'v' } }
    Zone $c 1 1 { param($x, $y) Fill $c $x $y ($x + 15) ($y + 15) 'f'; Noise $c $x $y ($x + 15) ($y + 15) 'g' 1 6 }
    Save $c $pal "$blocks/${name}_postbox.png"
}

# --- packages: a box seen at an angle, sized by style ----------------------------------------------
function Box($c, [int]$w, [int]$h, [char]$side, [char]$top, [char]$dark, [char]$tape, [char]$edge) {
    $x0 = [int][math]::Floor((16 - $w) / 2); $x1 = $x0 + $w - 1
    $y1 = 14; $y0 = $y1 - $h + 1
    $topH = [math]::Max(2, [int]($h / 4))
    Fill $c $x0 $y0 $x1 ($y0 + $topH - 1) $top
    Fill $c $x0 ($y0 + $topH) $x1 $y1 $side
    Fill $c ($x0 + [int]($w / 2)) ($y0 + $topH) $x1 $y1 $dark
    $mid = $x0 + [int]($w / 2) - 1
    Fill $c $mid $y0 ($mid + 1) $y1 $tape
    Frame $c $x0 $y0 $x1 $y1 $edge $edge
}
$boxPal = @{ s = '#b88a52'; t = '#d1a66a'; d = '#9a6f3d'; p = '#e4d2a6'; e = '#5e4123'; k = '#1c1c1c'; g = '#5fb34a'; h = '#3d7f2e'; r = '#c23b32'; b = '#3b69c4'; y = '#e3c13b'; w = '#efefef'; m = '#a74fc8'; c = '#3fb6b8'; o = '#e07c22'; n = '#2b2b2b' }
$sizes = [ordered]@{ '12x12' = @(12, 12); '10x12' = @(10, 12); '10x8' = @(10, 8); '12x10' = @(12, 10) }
foreach ($s in $sizes.Keys) {
    $c = New-Canvas; Box $c $sizes[$s][0] $sizes[$s][1] 's' 't' 'd' 'p' 'e'
    Save $c $boxPal "$items/cardboard_package_$s.png"
}
# rare boxes: 12x10 boxes in their own colours with a small emblem
$rare = [ordered]@{
    creeper = @('g', 'h', 'h', 'k'); darcy = @('b', 'w', 'n', 'y'); evan = @('o', 'y', 'n', 'w'); jinx = @('m', 'w', 'n', 'c')
    kryppers = @('c', 'w', 'n', 'm'); simi = @('y', 'w', 'n', 'b'); starlotte = @('n', 'y', 'k', 'y'); thunder = @('b', 'y', 'n', 'y')
    up = @('r', 'w', 'n', 'w'); vector = @('w', 'r', 'n', 'k')
}
foreach ($name in $rare.Keys) {
    $p = $rare[$name]
    $c = New-Canvas; Box $c 12 10 $p[0] $p[1] $p[2] $p[1] 'e'
    # emblem on the front
    switch ($name) {
        'creeper' { Fill $c 4 8 5 9 'k'; Fill $c 8 8 9 9 'k'; Fill $c 6 10 7 12 'k'; Set-Px $c 5 12 'k'; Set-Px $c 8 12 'k' }
        'starlotte' { Set-Px $c 5 8 'y'; Fill $c 4 9 6 9 'y'; Set-Px $c 5 10 'y' }
        'thunder' { Set-Px $c 6 7 'y'; Set-Px $c 5 8 'y'; Fill $c 5 9 7 9 'y'; Set-Px $c 6 10 'y'; Set-Px $c 5 11 'y' }
        'up' { Set-Px $c 5 8 'w'; Fill $c 4 9 6 9 'w'; Fill $c 5 10 5 12 'w' }
        default { Fill $c 4 9 6 11 $p[3] }
    }
    Save $c $boxPal "$items/rare_${name}_package.png"
}

# --- package filter: wool strip between zinc nuggets with an address tag -----------------------------
$filterPal = @{ w = '#d9d4c7'; x = '#b8b2a3'; z = '#a7b9b0'; y = '#7e918a'; t = '#b88a52'; u = '#8d6538'; k = '#3a3a3a' }
$c = New-Canvas
Fill $c 3 6 12 9 'w'; Noise $c 3 6 12 9 'x' 1 4
Fill $c 0 5 2 10 'z'; Fill $c 13 5 15 10 'z'; Fill $c 0 10 2 10 'y'; Fill $c 13 10 15 10 'y'
Fill $c 6 1 9 4 't'; Set-Px $c 7 2 'k'; Set-Px $c 8 2 'k'; Fill $c 6 4 9 4 'u'
Fill $c 6 11 9 14 't'; Fill $c 6 14 9 14 'u'
Save $c $filterPal "$items/package_filter.png"

Write-Host 'packages textures written'
