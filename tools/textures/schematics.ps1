# Textures of the schematics unit (U29): schematic items, schematic table, schematicannon and the
# crafting blueprint. Original pixel art.
#
#   pwsh tools/textures/schematics.ps1            # writes assets/pack/rp/textures/{blocks,items}/
#   pwsh tools/textures/schematics.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$blocks = Join-Path $root 'assets/pack/rp/textures/blocks'
$items = Join-Path $root 'assets/pack/rp/textures/items'
New-Item -ItemType Directory -Force $blocks, $items | Out-Null

function New-Canvas([char]$fill = '.') { , (0..15 | ForEach-Object { , ([char[]]((New-Object string $fill, 16))) }) }
function Set-Px($c, $x, $y, [char]$k) { if ($x -ge 0 -and $x -lt 16 -and $y -ge 0 -and $y -lt 16) { $c[$y][$x] = $k } }
function Fill($c, $x0, $y0, $x1, $y1, [char]$k) { for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y $k } } }
function Line($c, $x0, $y0, $x1, $y1, [char]$k) {
    $n = [math]::Max([math]::Abs($x1 - $x0), [math]::Abs($y1 - $y0))
    for ($i = 0; $i -le $n; $i++) {
        $t = if ($n -eq 0) { 0 } else { $i / $n }
        Set-Px $c ([math]::Round($x0 + ($x1 - $x0) * $t)) ([math]::Round($y0 + ($y1 - $y0) * $t)) $k
    }
}
function Rows($c) { , ($c | ForEach-Object { -join $_ }) }
function Save($c, $pal, $path) {
    $rows = Rows $c
    Save-PixelArt -Rows $rows -Palette $pal -Path $path
    if ($Preview) {
        $dir = Join-Path $root 'tools/textures/preview'
        New-Item -ItemType Directory -Force $dir | Out-Null
        Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $dir (Split-Path -Leaf $path)) -Scale 8
    }
}

# shared palette: blueprint paper, ink, wood, iron
$pal = @{
    b = '#2f6fb3'; e = '#4a8fd6'; l = '#7fb6ea'; g = '#bfe0ff'; w = '#f2f8ff'; d = '#1d4877'; n = '#123153'
    p = '#e9dfc4'; a = '#c9b98f'; o = '#a47b4a'; m = '#7c5a33'; r = '#5a3d20'; h = '#c49a62'
    i = '#9aa1a8'; c = '#c7ccd1'; k = '#5f656b'; z = '#3c4044'; s = '#7d7f7a'; t = '#a9aca5'; x = '#2b2d30'
    f = '#ffffff'; j = '#d8d8d8'; q = '#3a3a3a'; y = '#e3c25a'
}

# ---- items --------------------------------------------------------------------------------------
# a sheet of blueprint paper rolled at the top and bottom
function Paper($c, [char]$fill, [char]$edge) {
    Fill $c 3 2 12 13 $fill
    Fill $c 2 1 13 2 $edge; Fill $c 2 13 13 14 $edge
    Set-Px $c 2 1 'n'; Set-Px $c 13 1 'n'; Set-Px $c 2 14 'n'; Set-Px $c 13 14 'n'
    Fill $c 3 3 3 12 'd'; Fill $c 12 3 12 12 'd'
}
$c = New-Canvas; Paper $c 'l' 'e'
foreach ($y in 4, 7, 10) { Line $c 5 $y 10 $y 'g' }
Save $c $pal "$items/empty_schematic.png"

$c = New-Canvas; Paper $c 'b' 'e'
# grid and a little house drawn in white
foreach ($x in 5, 8, 11) { Line $c $x 3 $x 12 'd' }
foreach ($y in 5, 8, 11) { Line $c 4 $y 11 $y 'd' }
Line $c 5 8 7 5 'w'; Line $c 7 5 10 8 'w'; Line $c 5 8 5 11 'w'; Line $c 10 8 10 11 'w'; Line $c 5 11 10 11 'w'
Set-Px $c 7 10 'g'; Set-Px $c 7 11 'g'
Save $c $pal "$items/schematic.png"

$c = New-Canvas; Paper $c 'b' 'e'
foreach ($x in 5, 8, 11) { Line $c $x 3 $x 12 'd' }
foreach ($y in 5, 8, 11) { Line $c 4 $y 11 $y 'd' }
# quill: white feather from bottom-left to top-right, ink tip
Line $c 4 12 13 1 'f'; Line $c 5 12 13 2 'j'; Line $c 4 11 12 1 'j'
Set-Px $c 3 13 'q'; Set-Px $c 4 13 'q'; Set-Px $c 3 14 'x'
Save $c $pal "$items/schematic_and_quill.png"

$c = New-Canvas
Fill $c 2 2 13 13 'm'; Fill $c 3 3 12 12 'p'
Fill $c 2 2 13 2 'h'; Fill $c 2 2 2 13 'h'; Fill $c 13 3 13 13 'r'; Fill $c 3 13 13 13 'r'
foreach ($i in 0..2) { foreach ($j in 0..2) { Fill $c (4 + $i * 3) (4 + $j * 3) (5 + $i * 3) (5 + $j * 3) 'a' } }
Set-Px $c 5 5 'b'; Set-Px $c 8 8 'b'; Set-Px $c 11 11 'b'; Set-Px $c 11 5 'y'
Save $c $pal "$items/crafting_blueprint.png"

# ---- blocks -------------------------------------------------------------------------------------
# blueprint section: paper with a frame, grid and a sketched recipe arrow
$c = New-Canvas 'p'
Fill $c 0 0 15 0 'h'; Fill $c 0 0 0 15 'h'; Fill $c 15 0 15 15 'm'; Fill $c 0 15 15 15 'm'
Fill $c 1 1 14 1 'a'; Fill $c 1 1 1 14 'a'
foreach ($i in 0..2) { foreach ($j in 0..2) { Fill $c (3 + $i * 3) (3 + $j * 3) (4 + $i * 3) (4 + $j * 3) 'a' } }
Line $c 12 7 13 7 'm'; Set-Px $c 13 6 'm'; Set-Px $c 13 8 'm'
Save $c $pal "$blocks/crafting_blueprint.png"

# schematic table: drafting board (blue paper on a wooden frame), wooden sides
$c = New-Canvas 'b'
Fill $c 0 0 15 1 'o'; Fill $c 0 14 15 15 'o'; Fill $c 0 0 1 15 'o'; Fill $c 14 0 15 15 'o'
Fill $c 0 0 15 0 'h'; Fill $c 0 15 15 15 'r'
foreach ($x in 4, 7, 10) { Line $c $x 2 $x 13 'e' }
foreach ($y in 5, 8, 11) { Line $c 2 $y 13 $y 'e' }
Line $c 4 10 7 4 'w'; Line $c 7 4 11 9 'w'; Line $c 4 10 11 10 'g'
Fill $c 11 11 12 12 'y'
Save $c $pal "$blocks/schematic_table_top.png"

$c = New-Canvas 'o'
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
    if ($y % 4 -eq 3) { Set-Px $c $x $y 'm' } elseif ((($x * 5 + $y * 3) % 13) -eq 0) { Set-Px $c $x $y 'h' }
} }
Fill $c 0 0 0 15 'r'; Fill $c 15 0 15 15 'r'
Save $c $pal "$blocks/schematic_table_side.png"

# schematicannon: stone-and-iron base, iron body, dark barrel
$c = New-Canvas 's'
Fill $c 0 0 15 0 't'; Fill $c 0 15 15 15 'k'; Fill $c 0 0 0 15 't'; Fill $c 15 0 15 15 'k'
for ($y = 2; $y -lt 15; $y += 4) { Line $c 1 $y 14 $y 'k' }
foreach ($p in @(@(3, 4), @(12, 4), @(3, 12), @(12, 12))) { Set-Px $c $p[0] $p[1] 'c' }
Save $c $pal "$blocks/schematicannon_base.png"

$c = New-Canvas 'i'
Fill $c 0 0 15 1 'c'; Fill $c 0 14 15 15 'k'
for ($x = 1; $x -lt 16; $x += 5) { Fill $c $x 2 $x 13 'k' }
foreach ($p in @(@(2, 3), @(13, 3), @(2, 12), @(13, 12))) { Set-Px $c $p[0] $p[1] 'z' }
Save $c $pal "$blocks/schematicannon_body.png"

$c = New-Canvas 'z'
Fill $c 0 0 15 0 'k'; Fill $c 0 15 15 15 'x'
for ($y = 2; $y -lt 16; $y += 6) { Fill $c 0 $y 15 $y 'k'; Fill $c 0 ($y + 1) 15 ($y + 1) 'i' }
Fill $c 5 5 10 10 'x'; Fill $c 6 6 9 9 'q'
Save $c $pal "$blocks/schematicannon_barrel.png"

Write-Host 'schematics textures written'
