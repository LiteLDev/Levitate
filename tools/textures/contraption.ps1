# Textures of the contraption unit (U4): chassis, sticker, super glue. Original pixel art.
#
#   pwsh tools/textures/contraption.ps1            # writes assets/pack/rp/textures/{blocks,items}/
#   pwsh tools/textures/contraption.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$blocks = Join-Path $root 'assets/pack/rp/textures/blocks'
$items = Join-Path $root 'assets/pack/rp/textures/items'

# --- tiny canvas helpers (16x16 char grids) -------------------------------------------------------
function New-Canvas([char]$fill = '.') { , (0..15 | ForEach-Object { , ([char[]]((New-Object string $fill, 16))) }) }
function Set-Px($c, $x, $y, [char]$k) { if ($x -ge 0 -and $x -lt 16 -and $y -ge 0 -and $y -lt 16) { $c[$y][$x] = $k } }
function Fill($c, $x0, $y0, $x1, $y1, [char]$k) { for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y $k } } }
function Rows($c) { , ($c | ForEach-Object { -join $_ }) }
function Save($c, $pal, $path) {
    $rows = Rows $c
    Save-PixelArt -Rows $rows -Palette $pal -Path $path
    if ($Preview) { Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $root "tools/textures/preview/$(Split-Path -Leaf $path)") -Scale 8 }
}

function Merge-Pal($base, $over) { $h = @{}; foreach ($k in $base.Keys) { $h[$k] = $base[$k] }; foreach ($k in $over.Keys) { $h[$k] = $over[$k] }; $h }
# planks with a 2-px frame: the chassis body
function Chassis-Body($c) {
    for ($y = 0; $y -lt 16; $y++) {
        $band = [math]::Floor($y / 4)
        for ($x = 0; $x -lt 16; $x++) {
            $k = if ((($x + $band * 5) % 16) -eq 0) { 'g' } elseif ($y % 4 -eq 3) { 'd' } elseif (($x * 7 + $y * 3) % 11 -eq 0) { 'l' } else { 'p' }
            Set-Px $c $x $y $k
        }
    }
    Fill $c 0 0 15 1 'F'; Fill $c 0 14 15 15 'F'; Fill $c 0 0 1 15 'F'; Fill $c 14 0 15 15 'F'
    Fill $c 0 0 15 0 'H'; Fill $c 0 0 0 15 'H'; Fill $c 0 15 15 15 'S'; Fill $c 15 0 15 15 'S'
    foreach ($p in @(@(2, 2), @(13, 2), @(2, 13), @(13, 13))) { Set-Px $c $p[0] $p[1] 'n' }
}
# end face: frame with a lattice (glueable face)
function Chassis-End($c, [switch]$Sticky, [switch]$Ring) {
    Chassis-Body $c
    if ($Ring) {
        foreach ($y in 4..11) { foreach ($x in 4..11) {
            $dx = $x - 7.5; $dy = $y - 7.5; $r = [math]::Sqrt($dx * $dx + $dy * $dy)
            if ($r -ge 2.6 -and $r -le 3.9) { Set-Px $c $x $y 'F' }
        } }
        Set-Px $c 7 7 'H'; Set-Px $c 8 8 'S'; Set-Px $c 7 8 'F'; Set-Px $c 8 7 'F'
    } else {
        foreach ($i in 2..13) { Set-Px $c $i 5 'F'; Set-Px $c $i 10 'F'; Set-Px $c 5 $i 'F'; Set-Px $c 10 $i 'F' }
        foreach ($i in 2..13) { Set-Px $c $i 6 'S'; Set-Px $c $i 11 'S' }
    }
    if ($Sticky) {
        for ($y = 2; $y -le 13; $y++) { for ($x = 2; $x -le 13; $x++) {
            if ((($x + $y) % 3) -ne 0) { Set-Px $c $x $y ($(if (($x * 3 + $y) % 5 -eq 0) { 'Z' } else { 'Y' })) }
        } }
        foreach ($p in @(@(4, 4), @(9, 6), @(6, 10), @(11, 11))) { Set-Px $c $p[0] $p[1] 'w' }
    }
}

$frame = @{ F = '#7d807a'; H = '#a9aca5'; S = '#55574f'; n = '#3c3d38'; Y = '#7fcb5e'; Z = '#5c9f45'; w = '#c9f2b0' }
$oak = Merge-Pal $frame @{ p = '#a47b4a'; l = '#b88f5a'; d = '#7c5a33'; g = '#6b4c2a' }
$spruce = Merge-Pal $frame @{ p = '#6e4e2e'; l = '#83603a'; d = '#4f3620'; g = '#432c19'; F = '#9a7a3a'; H = '#c9a85a'; S = '#6d5326' }
$radial = Merge-Pal $frame @{ p = '#9c6f45'; l = '#b0845a'; d = '#74512f'; g = '#5f4227' }

foreach ($set in @(
        @{ name = 'linear_chassis'; pal = $oak; ring = $false },
        @{ name = 'secondary_linear_chassis'; pal = $spruce; ring = $false })) {
    $c = New-Canvas; Chassis-Body $c; Save $c $set.pal "$blocks/$($set.name)_side.png"
    $c = New-Canvas; Chassis-End $c; Save $c $set.pal "$blocks/$($set.name)_end.png"
    $c = New-Canvas; Chassis-End $c -Sticky; Save $c $set.pal "$blocks/$($set.name)_end_sticky.png"
}
# radial: the four sides are glueable, the two axis ends show a ring
$c = New-Canvas; Chassis-End $c -Ring; Save $c $radial "$blocks/radial_chassis_end.png"
$c = New-Canvas; Chassis-Body $c; foreach ($i in 3..12) { Set-Px $c $i 7 'F'; Set-Px $c $i 8 'S' }; Save $c $radial "$blocks/radial_chassis_side.png"
$c = New-Canvas; Chassis-Body $c; foreach ($i in 3..12) { Set-Px $c $i 7 'F'; Set-Px $c $i 8 'S' }
for ($y = 2; $y -le 13; $y++) { for ($x = 2; $x -le 13; $x++) { if ((($x + 2 * $y) % 3) -ne 0) { Set-Px $c $x $y ($(if (($x + $y) % 4 -eq 0) { 'Z' } else { 'Y' })) } } }
Save $c $radial "$blocks/radial_chassis_side_sticky.png"

# --- sticker: andesite casing with a slime pad on the front --------------------------------------
$andesite = @{ a = '#9a9d96'; b = '#80837c'; c = '#686b64'; e = '#b9bcb4'; k = '#4a4c47'; Y = '#7fcb5e'; Z = '#5c9f45'; w = '#c9f2b0'; v = '#3f6b30'; o = '#8b6a3e' }
function Casing($c) {
    for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { Set-Px $c $x $y ($(if ((($x * 5 + $y * 3) % 7) -eq 0) { 'b' } else { 'a' })) } }
    Fill $c 0 0 15 0 'e'; Fill $c 0 0 0 15 'e'; Fill $c 0 15 15 15 'k'; Fill $c 15 0 15 15 'k'
    Fill $c 1 1 14 1 'c'; Fill $c 1 14 14 14 'c'
}
$c = New-Canvas; Casing $c; Fill $c 3 6 12 9 'o'; Fill $c 3 6 12 6 'c'; Save $c $andesite "$blocks/sticker_side.png"
$c = New-Canvas; Casing $c; Fill $c 5 5 10 10 'c'; Fill $c 6 6 9 9 'k'; Save $c $andesite "$blocks/sticker_back.png"
$c = New-Canvas; Casing $c; Fill $c 3 3 12 12 'c'; Fill $c 4 4 11 11 'v'; Fill $c 5 5 10 10 'Z'; Save $c $andesite "$blocks/sticker_front.png"
$c = New-Canvas; Casing $c; Fill $c 2 2 13 13 'Z'; Fill $c 3 3 12 12 'Y'; Set-Px $c 5 5 'w'; Set-Px $c 6 5 'w'; Set-Px $c 5 6 'w'; Set-Px $c 10 9 'Z'; Save $c $andesite "$blocks/sticker_front_extended.png"

# --- super glue (item): a squeezed tube with a green cap ------------------------------------------
$glue = @(
    '................',
    '..........CE....',
    '.........CEEC...',
    '........kCECk...',
    '.......kBBkk....',
    '......kBBBBk....',
    '.....kBBRBBk....',
    '....kBBRBBk.....',
    '...kBBRBBk......',
    '..kBBRBBk.......',
    '..kBRBBk........',
    '..kBBBk.........',
    '...kkk..........',
    '..d.............',
    '.dd.............',
    '................'
)
$gluePal = @{ k = '#3d3f45'; B = '#e8ece4'; R = '#b9c0b5'; C = '#4f9a3a'; E = '#7fcb5e'; d = '#d8f0c8' }
Save-PixelArt -Rows $glue -Palette $gluePal -Path "$items/super_glue.png"
if ($Preview) { Save-PixelArt -Rows $glue -Palette $gluePal -Path (Join-Path $root 'tools/textures/preview/super_glue.png') -Scale 8 }
Write-Host 'contraption textures written'
