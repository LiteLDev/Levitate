# Textures of the equipment unit (U31): toolboxes (16 colours), backtanks, diving gear, held tools,
# the lectern controller and the worn armour layers. Original pixel art.
#
#   pwsh tools/textures/equipment.ps1            # writes assets/pack/rp/textures/{blocks,items,models/armor}/
#   pwsh tools/textures/equipment.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$blocks = Join-Path $root 'assets/pack/rp/textures/blocks'
$items = Join-Path $root 'assets/pack/rp/textures/items'
$armor = Join-Path $root 'assets/pack/rp/textures/models/armor'

# --- canvas helpers (any size; char grids) --------------------------------------------------------------
function New-Canvas([int]$w = 16, [int]$h = 16, [char]$fill = '.') { , (0..($h - 1) | ForEach-Object { , ([char[]]((New-Object string $fill, $w))) }) }
function Set-Px($c, $x, $y, [char]$k) { if ($y -ge 0 -and $y -lt $c.Count -and $x -ge 0 -and $x -lt $c[0].Count) { $c[$y][$x] = $k } }
function Fill($c, $x0, $y0, $x1, $y1, [char]$k) { for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y $k } } }
function Frame($c, $x0, $y0, $x1, $y1, [char]$k) { for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y0 $k; Set-Px $c $x $y1 $k }; for ($y = $y0; $y -le $y1; $y++) { Set-Px $c $x0 $y $k; Set-Px $c $x1 $y $k } }
function Rows($c) { , ($c | ForEach-Object { -join $_ }) }
function Save($c, $pal, $path) {
    $rows = Rows $c
    Save-PixelArt -Rows $rows -Palette $pal -Path $path
    if ($Preview) { Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $root "tools/textures/preview/$(Split-Path -Leaf $path)") -Scale 8 }
}
function Merge-Pal($base, $over) { $h = @{}; foreach ($k in $base.Keys) { $h[$k] = $base[$k] }; foreach ($k in $over.Keys) { $h[$k] = $over[$k] }; $h }
function Hex($color) { $x = ConvertTo-Color $color; '#{0:x2}{1:x2}{2:x2}' -f $x.R, $x.G, $x.B }

# Box-UV region of a cube (size w, h, d at u, v): fills every face with $k, frames each face with $e.
function Paint-Cube($c, $u, $v, $w, $h, $d, [char]$k, [char]$e) {
    Fill $c ($u + $d) $v ($u + $d + 2 * $w - 1) ($v + $d - 1) $k            # top + bottom
    Fill $c $u ($v + $d) ($u + 2 * $d + 2 * $w - 1) ($v + $d + $h - 1) $k   # sides
    $faces = @(@(($u + $d), $v, $w, $d), @(($u + $d + $w), $v, $w, $d), @($u, ($v + $d), $d, $h), @(($u + $d), ($v + $d), $w, $h),
        @(($u + $d + $w), ($v + $d), $d, $h), @(($u + 2 * $d + $w), ($v + $d), $w, $h))
    foreach ($f in $faces) { if ($f[2] -gt 1 -and $f[3] -gt 1) { Frame $c $f[0] $f[1] ($f[0] + $f[2] - 1) ($f[1] + $f[3] - 1) $e } }
}
# a horizontal band on the side faces of a cube (rows from the top of the sides)
function Side-Band($c, $u, $v, $w, $h, $d, $row, [char]$k) { $y = $v + $d + $row; for ($x = $u; $x -lt $u + 2 * $d + 2 * $w; $x++) { Set-Px $c $x $y $k } }

# --- dye colours -------------------------------------------------------------------------------------------
$dyes = [ordered]@{
    white = '#e4e6e6'; orange = '#e8761c'; magenta = '#b84ab0'; light_blue = '#46aedb'; yellow = '#efc32e'; lime = '#76b828'
    pink = '#e78fae'; gray = '#4b5256'; light_gray = '#8f908a'; cyan = '#1a8b93'; purple = '#7b33ad'; blue = '#3a40a0'
    brown = '#7a4e2c'; green = '#566f22'; red = '#a83029'; black = '#24252a'
}
$brass = @{ b = '#d9a548'; h = '#f2d27a'; g = '#a8742c'; k = '#2b2119' }

# --- toolboxes ------------------------------------------------------------------------------------------------
foreach ($name in $dyes.Keys) {
    $dye = $dyes[$name]
    $pal = Merge-Pal $brass @{ x = (Hex $dye); y = (Hex (Shade-Color $dye 1.25)); z = (Hex (Shade-Color $dye 0.7)); o = (Hex (Shade-Color $dye 0.5)) }
    # block texture (64x64, box UV of geometry.levitate.toolbox)
    $c = New-Canvas 64 64
    Paint-Cube $c 0 0 14 8 8 'x' 'o'
    Side-Band $c 0 0 14 8 8 1 'b'; Side-Band $c 0 0 14 8 8 6 'z'
    Paint-Cube $c 0 16 14 2 8 'y' 'o'
    Side-Band $c 0 16 14 2 8 1 'g'
    Paint-Cube $c 0 28 1 2 2 'g' 'k'; Paint-Cube $c 8 28 1 2 2 'g' 'k'; Paint-Cube $c 16 28 6 1 2 'b' 'g'; Paint-Cube $c 36 28 2 3 1 'h' 'g'
    Save $c $pal "$blocks/${name}_toolbox.png"
    # item icon
    $c = New-Canvas
    Frame $c 5 2 10 4 'k'; Fill $c 6 3 9 3 '.'; Set-Px $c 5 4 'g'; Set-Px $c 10 4 'g'
    Fill $c 2 5 13 12 'k'
    Fill $c 3 6 12 7 'y'; Fill $c 3 8 12 8 'b'; Fill $c 3 9 12 11 'x'; Fill $c 3 11 12 11 'z'
    Fill $c 7 8 8 10 'h'; Set-Px $c 7 10 'g'; Set-Px $c 8 10 'g'
    Save $c $pal "$items/${name}_toolbox.png"
}

# --- backtanks ---------------------------------------------------------------------------------------------
$copper = @{ c = '#c8754e'; l = '#e69a72'; d = '#93502f'; o = '#5e3320'; s = '#4a3a2c'; q = '#6b5640'; g = '#b9c4c4'; j = '#ecf4f4'; k = '#25201c'; a = '#9a9d96' }
$nether = @{ c = '#4d474d'; l = '#6e676e'; d = '#332e33'; o = '#1f1b1f'; s = '#3a2f2a'; q = '#5c4a40'; g = '#b9c4c4'; j = '#ecf4f4'; k = '#100d10'; a = '#6a6d66' }
foreach ($set in @(@{ name = 'copper'; pal = $copper }, @{ name = 'netherite'; pal = $nether })) {
    $c = New-Canvas 64 64
    Paint-Cube $c 0 0 5 12 6 'c' 'o'; Side-Band $c 0 0 5 12 6 1 'l'; Side-Band $c 0 0 5 12 6 10 'd'
    Paint-Cube $c 24 0 5 12 6 'c' 'o'; Side-Band $c 24 0 5 12 6 1 'l'; Side-Band $c 24 0 5 12 6 10 'd'
    Paint-Cube $c 0 20 3 2 4 'd' 'o'; Paint-Cube $c 16 20 3 2 4 'd' 'o'
    Paint-Cube $c 0 28 14 2 8 's' 'k'; Side-Band $c 0 28 14 2 8 0 'q'
    Paint-Cube $c 0 40 4 2 4 'a' 'k'
    Paint-Cube $c 20 40 2 2 1 'g' 'k'; Set-Px $c 22 41 'j'
    Save $c $set.pal "$blocks/$($set.name)_backtank.png"
    $c = New-Canvas
    Fill $c 3 2 7 13 'o'; Fill $c 8 2 12 13 'o'
    Fill $c 4 3 6 12 'c'; Fill $c 9 3 11 12 'c'; Fill $c 4 3 4 12 'l'; Fill $c 9 3 9 12 'l'; Fill $c 6 4 6 12 'd'; Fill $c 11 4 11 12 'd'
    Fill $c 4 1 6 1 'd'; Fill $c 9 1 11 1 'd'
    Fill $c 2 7 13 8 's'; Fill $c 2 7 13 7 'q'
    Fill $c 7 10 8 11 'g'; Set-Px $c 7 10 'j'
    Save $c $set.pal "$items/$($set.name)_backtank.png"
    # worn layer (64x32 armour layout): chest + arms copper with straps, tanks on the back
    $c = New-Canvas 64 32
    Fill $c 16 16 39 31 'c'; Fill $c 40 16 55 31 'c'
    Fill $c 16 20 39 21 's'; Fill $c 40 20 55 21 's'
    Fill $c 32 20 39 31 'd'; Fill $c 33 21 34 30 'l'; Fill $c 37 21 38 30 'l'
    Frame $c 20 20 27 31 'o'
    Save $c $set.pal "$armor/$($set.name)_backtank.png"
}

# --- diving helmets and boots -----------------------------------------------------------------------------------
$glass = @{ w = '#9fd3e6'; v = '#d8f1fa' }
foreach ($set in @(@{ name = 'copper'; pal = (Merge-Pal $copper $glass) }, @{ name = 'netherite'; pal = (Merge-Pal $nether $glass) })) {
    $c = New-Canvas
    Fill $c 3 2 12 13 'o'; Fill $c 4 3 11 12 'c'; Fill $c 4 3 11 4 'l'; Fill $c 4 11 11 12 'd'
    Fill $c 5 6 10 10 'k'; Fill $c 6 7 9 9 'w'; Set-Px $c 6 7 'v'; Set-Px $c 7 7 'v'
    Fill $c 2 13 13 14 'd'
    Save $c $set.pal "$items/$($set.name)_diving_helmet.png"
    $c = New-Canvas
    Fill $c 2 4 6 13 'o'; Fill $c 9 4 13 13 'o'; Fill $c 3 5 5 12 'c'; Fill $c 10 5 12 12 'c'
    Fill $c 1 11 6 13 'o'; Fill $c 9 11 14 13 'o'; Fill $c 2 12 5 12 'd'; Fill $c 10 12 13 12 'd'
    Fill $c 3 5 5 5 'l'; Fill $c 10 5 12 5 'l'; Fill $c 1 13 6 13 'a'; Fill $c 9 13 14 13 'a'
    Save $c $set.pal "$items/$($set.name)_diving_boots.png"
    # helmet layer: head 8x8x8 at uv 0,0 with a round window in front (u 8..15, v 8..15)
    $c = New-Canvas 64 32
    Fill $c 0 0 31 15 'c'; Fill $c 8 0 23 7 'l'; Fill $c 0 14 31 15 'd'
    Fill $c 9 9 14 14 'k'; Fill $c 10 10 13 13 'w'; Set-Px $c 10 10 'v'; Set-Px $c 11 10 'v'
    Fill $c 0 10 7 11 'g'; Fill $c 16 10 23 11 'g'
    Save $c $set.pal "$armor/$($set.name)_diving_helmet.png"
    # boots layer: legs at uv 0,16
    $c = New-Canvas 64 32
    Fill $c 0 16 15 31 'c'; Fill $c 0 26 15 31 'd'; Fill $c 0 29 15 31 'a'
    Save $c $set.pal "$armor/$($set.name)_diving_boots.png"
}

# --- held tools ------------------------------------------------------------------------------------------------------
$wood = @{ s = '#8a6a3e'; u = '#b08d58'; t = '#5c4426' }
$metal = @{ m = '#9a9d96'; i = '#c9ccc4'; n = '#5d605a'; b = '#d9a548'; h = '#f2d27a'; g = '#a8742c'; k = '#25201c' }
# extendo grip: a long scissor arm with a hand
$c = New-Canvas
foreach ($i in 0..9) { Set-Px $c (2 + $i) (13 - $i) 's'; Set-Px $c (3 + $i) (13 - $i) 't' }
foreach ($i in 0..3) { Set-Px $c (4 + 2 * $i) (11 - 2 * $i) 'b' }
Fill $c 11 1 14 4 'k'; Fill $c 12 2 13 3 'h'; Set-Px $c 14 1 'b'; Set-Px $c 11 1 'b'
Fill $c 1 13 3 15 'g'; Set-Px $c 2 14 'h'
Save $c (Merge-Pal $wood $metal) "$items/extendo_grip.png"
# potato cannon: a copper barrel on an andesite grip
$c = New-Canvas
Fill $c 3 5 14 9 'k'; Fill $c 4 6 13 8 'c'; Fill $c 4 6 13 6 'l'; Fill $c 4 8 13 8 'd'
Fill $c 14 6 15 8 'n'; Fill $c 2 9 6 13 'k'; Fill $c 3 10 5 12 'm'; Fill $c 3 10 3 12 'i'
Fill $c 8 9 9 11 'n'; Set-Px $c 1 7 'm'
Save $c (Merge-Pal $copper $metal) "$items/potato_cannon.png"
# wand of symmetry: a brass rod with a glowing crystal
$c = New-Canvas
foreach ($i in 0..8) { Set-Px $c (3 + $i) (14 - $i) 'b'; Set-Px $c (4 + $i) (14 - $i) 'g' }
Fill $c 10 2 14 6 'k'; Fill $c 11 3 13 5 'w'; Set-Px $c 11 3 'v'; Set-Px $c 12 1 'v'; Set-Px $c 15 4 'v'; Set-Px $c 9 4 'v'
Fill $c 2 13 4 15 'n'
Save $c (Merge-Pal $metal @{ w = '#b48ef0'; v = '#efe2ff' }) "$items/wand_of_symmetry.png"
# handheld worldshaper: a dark frame around a blue core
$c = New-Canvas
Fill $c 2 4 13 10 'k'; Fill $c 3 5 12 9 'p'; Fill $c 5 6 10 8 'w'; Fill $c 6 7 7 7 'v'
Fill $c 13 6 15 8 'n'; Fill $c 3 10 6 14 'k'; Fill $c 4 11 5 13 'p'; Set-Px $c 9 4 'h'; Set-Px $c 11 4 'h'
Save $c (Merge-Pal $metal @{ p = '#4b3b68'; w = '#5fd2f0'; v = '#d8f8ff' }) "$items/handheld_worldshaper.png"
# linked controller: a wooden pad with six buttons
$c = New-Canvas
Fill $c 1 4 14 11 'k'; Fill $c 2 5 13 10 's'; Fill $c 2 5 13 5 'u'; Fill $c 2 10 13 10 't'
Set-Px $c 4 6 'r'; Set-Px $c 3 7 'r'; Set-Px $c 5 7 'r'; Set-Px $c 4 8 'r'
Set-Px $c 10 7 'e'; Set-Px $c 12 7 'e'; Fill $c 7 6 8 7 'b'; Set-Px $c 7 9 'n'; Set-Px $c 8 9 'n'
Save $c (Merge-Pal (Merge-Pal $wood $metal) @{ r = '#c23a2c'; e = '#e8574a' }) "$items/linked_controller.png"
# sand papers
foreach ($set in @(@{ name = 'sand_paper'; pal = @{ p = '#e6dcae'; f = '#f6f0d2'; q = '#c9bd88'; k = '#7d7350' } },
        @{ name = 'red_sand_paper'; pal = @{ p = '#d48b52'; f = '#ecb486'; q = '#b0693a'; k = '#6b3d1f' } })) {
    $c = New-Canvas
    Fill $c 3 2 12 13 'k'; Fill $c 4 3 11 12 'p'; Fill $c 4 3 11 3 'f'
    foreach ($p in @(@(5, 5), @(8, 4), @(10, 7), @(6, 9), @(9, 11), @(5, 12), @(11, 10), @(7, 7))) { Set-Px $c $p[0] $p[1] 'q' }
    Fill $c 12 3 12 4 'f'
    Save $c $set.pal "$items/$($set.name).png"
}
# tree fertilizer: a green pouch with a sprout
$c = New-Canvas
Fill $c 4 6 11 13 'k'; Fill $c 5 7 10 12 'f'; Fill $c 5 7 10 7 'a'; Fill $c 5 12 10 12 'e'
Fill $c 6 5 9 6 't'; Set-Px $c 7 4 'l'; Set-Px $c 8 3 'l'; Set-Px $c 6 3 'y'; Set-Px $c 9 2 'y'
Set-Px $c 7 9 'w'; Set-Px $c 8 10 'w'
Save $c (Merge-Pal $wood @{ f = '#5c8a3a'; a = '#7fb054'; e = '#3e5f27'; k = '#26331b'; l = '#4f9a2e'; y = '#8fd45e'; w = '#efe9d6' }) "$items/tree_fertilizer.png"

# --- lectern controller (64x64, box UV of geometry.levitate.lectern_controller) ------------------------------
$c = New-Canvas 64 64
Paint-Cube $c 0 0 16 2 16 'p' 't'
Paint-Cube $c 0 18 8 10 8 's' 't'; Side-Band $c 0 18 8 10 8 4 'u'
Paint-Cube $c 0 38 16 2 12 'u' 't'
Paint-Cube $c 32 18 8 2 5 'k' 'n'; Set-Px $c 38 19 'r'; Set-Px $c 41 19 'r'; Set-Px $c 44 19 'b'
Save $c (Merge-Pal (Merge-Pal $wood $metal) @{ p = '#9c7848'; r = '#c23a2c' }) "$blocks/lectern_controller.png"

Write-Host 'equipment textures written'
