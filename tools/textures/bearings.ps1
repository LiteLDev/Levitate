# Textures of the bearings unit (U21): bearings, turntable, sails, cart assembler, controller rail,
# minecart coupling and minecart contraption. Original pixel art.
#
#   pwsh tools/textures/bearings.ps1            # writes assets/pack/rp/textures/{blocks,items}/
#   pwsh tools/textures/bearings.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
#
# Model atlases (32x32) follow the UV layout of the generated geometry:
#   bearings:   side (0,0) | front (16,0) | back (0,16) | plate side (16,16, 12 wide) | shaft (28,16, 4 wide)
#   turntable:  side (0,0) | plate top (16,0) | bottom (0,16) | plate side (16,16)
#   assembler:  frame side (0,0) | frame top (16,0) | bed (0,16)
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$blocks = Join-Path $root 'assets/pack/rp/textures/blocks'
$items = Join-Path $root 'assets/pack/rp/textures/items'

# --- canvas helpers (w x h char grids) -------------------------------------------------------------
function New-Canvas([int]$w = 16, [int]$h = 16, [char]$fill = '.') { , (0..($h - 1) | ForEach-Object { , ([char[]]((New-Object string $fill, $w))) }) }
function Set-Px($c, $x, $y, [char]$k) { if ($y -ge 0 -and $y -lt $c.Count -and $x -ge 0 -and $x -lt $c[0].Count) { $c[$y][$x] = $k } }
function Fill($c, $x0, $y0, $x1, $y1, [char]$k) { for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y $k } } }
function Rows($c) { , ($c | ForEach-Object { -join $_ }) }
function Save($c, $pal, $path) {
    $rows = Rows $c
    Save-PixelArt -Rows $rows -Palette $pal -Path $path
    if ($Preview) { Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $root "tools/textures/preview/$(Split-Path -Leaf $path)") -Scale 8 }
}
function Merge-Pal($base, $over) { $h = @{}; foreach ($k in $base.Keys) { $h[$k] = $base[$k] }; foreach ($k in $over.Keys) { $h[$k] = $over[$k] }; $h }

# casing texture in a region: speckled fill, light top-left edge, dark bottom-right edge
function Casing($c, $x0, $y0, $w, $h) {
    for ($y = 0; $y -lt $h; $y++) { for ($x = 0; $x -lt $w; $x++) {
        Set-Px $c ($x0 + $x) ($y0 + $y) ($(if ((($x * 5 + $y * 3) % 7) -eq 0) { 'b' } else { 'a' }))
    } }
    Fill $c $x0 $y0 ($x0 + $w - 1) $y0 'e'; Fill $c $x0 $y0 $x0 ($y0 + $h - 1) 'e'
    Fill $c $x0 ($y0 + $h - 1) ($x0 + $w - 1) ($y0 + $h - 1) 'k'; Fill $c ($x0 + $w - 1) $y0 ($x0 + $w - 1) ($y0 + $h - 1) 'k'
}
# wooden planks running along x
function Planks($c, $x0, $y0, $w, $h) {
    for ($y = 0; $y -lt $h; $y++) { for ($x = 0; $x -lt $w; $x++) {
        $k = if ($y % 4 -eq 3) { 'd' } elseif (((($x + [math]::Floor($y / 4) * 7) % 13) -eq 0)) { 'g' } elseif ((($x * 3 + $y * 5) % 11) -eq 0) { 'l' } else { 'p' }
        Set-Px $c ($x0 + $x) ($y0 + $y) $k
    } }
}
function Rivets($c, $x0, $y0, $w, $h) {
    foreach ($p in @(@(2, 2), @(($w - 3), 2), @(2, ($h - 3)), @(($w - 3), ($h - 3)))) { Set-Px $c ($x0 + $p[0]) ($y0 + $p[1]) 'n' }
}

$wood = @{ p = '#a47b4a'; l = '#c09560'; d = '#6f4f2c'; g = '#5c4024' }
$metal = @{ s = '#9a9a9a'; t = '#6c6c6c'; u = '#c8c8c8'; h = '#2c2a26' }

function Bearing-Atlas($pal, $path, [switch]$Clock, [switch]$Sail) {
    $c = New-Canvas 32 32
    # side: casing band with the plate's edge on top (the casing faces sample its top rows)
    Casing $c 0 0 16 16; Rivets $c 0 0 16 10; Fill $c 0 9 15 9 'k'; Fill $c 1 10 14 14 'c'
    # front: the turning plate
    Planks $c 16 0 16 16
    Fill $c 16 0 31 0 'd'; Fill $c 16 15 31 15 'd'; Fill $c 16 0 16 15 'd'; Fill $c 31 0 31 15 'd'
    if ($Clock) {
        # clockwork: a dial ring
        foreach ($y in 0..15) { foreach ($x in 0..15) { $dx = $x - 7.5; $dy = $y - 7.5; $r = [math]::Sqrt($dx * $dx + $dy * $dy); if ($r -ge 5.2 -and $r -le 6.3) { Set-Px $c (16 + $x) $y 'q' } } }
        foreach ($p in @(@(7, 2), @(8, 2), @(13, 7), @(13, 8), @(7, 13), @(8, 13), @(2, 7), @(2, 8))) { Set-Px $c (16 + $p[0]) $p[1] 'z' }
    } elseif ($Sail) {
        # windmill: a cross to fix the sails on
        Fill $c 23 2 24 13 'g'; Fill $c 18 7 29 8 'g'
    }
    Fill $c 22 6 25 9 's'; Fill $c 23 7 24 8 't'
    # back: casing with the shaft hole
    Casing $c 0 16 16 16; Rivets $c 0 16 16 16; Fill $c 5 21 10 26 'k'; Fill $c 6 22 9 25 'h'
    # plate side and shaft
    Planks $c 16 16 12 16; Fill $c 16 16 27 16 'l'
    for ($y = 16; $y -lt 32; $y++) { Set-Px $c 28 $y 'u'; Set-Px $c 29 $y 's'; Set-Px $c 30 $y 's'; Set-Px $c 31 $y 't' }
    Save $c $pal $path
}

$andesite = Merge-Pal (Merge-Pal $wood $metal) @{ a = '#a49e8a'; b = '#8e8975'; c = '#7b7665'; e = '#c2bca8'; k = '#5b574a'; n = '#3e3b33'; q = '#3e3b33'; z = '#e8e0c8' }
$stone = Merge-Pal (Merge-Pal $wood $metal) @{ a = '#8f8f8f'; b = '#7a7a7a'; c = '#6a6a6a'; e = '#b0b0b0'; k = '#4e4e4e'; n = '#333333'; q = '#333333'; z = '#e0e0e0' }
$brass = Merge-Pal (Merge-Pal $wood $metal) @{ a = '#c9a24a'; b = '#b08a36'; c = '#8f6c26'; e = '#e7c46a'; k = '#6b4f1a'; n = '#4a3510'; q = '#3a2a0c'; z = '#f6e2a0' }
Bearing-Atlas $andesite "$blocks/mechanical_bearing_model.png"
Bearing-Atlas $stone "$blocks/windmill_bearing_model.png" -Sail
Bearing-Atlas $brass "$blocks/clockwork_bearing_model.png" -Clock

# --- turntable -----------------------------------------------------------------------------------------
$c = New-Canvas 32 32
Casing $c 0 0 16 16; Rivets $c 0 0 16 13; Fill $c 0 12 15 12 'k'
Planks $c 16 0 16 16
foreach ($y in 0..15) { foreach ($x in 0..15) {
    $dx = $x - 7.5; $dy = $y - 7.5; $r = [math]::Sqrt($dx * $dx + $dy * $dy)
    if ($r -gt 7.2) { Set-Px $c (16 + $x) $y 'd' } elseif ($r -ge 5.6 -and $r -le 6.4) { Set-Px $c (16 + $x) $y 'g' } elseif ($r -le 1.6) { Set-Px $c (16 + $x) $y 's' }
} }
Fill $c 23 2 24 4 'l'; Fill $c 23 11 24 13 'l'; Fill $c 18 7 20 8 'l'; Fill $c 27 7 29 8 'l'
Casing $c 0 16 16 16; Fill $c 6 22 9 25 'h'
Planks $c 16 16 16 16; Fill $c 16 16 31 16 'l'
Save $c $andesite "$blocks/turntable_model.png"

# --- cart assembler: an andesite frame around the rail ----------------------------------------------------
$c = New-Canvas 32 32
Casing $c 0 0 16 16; Fill $c 0 7 15 8 'c'; Rivets $c 0 0 16 16
Casing $c 16 0 16 16
Casing $c 0 16 16 16; Fill $c 2 18 13 29 'c'
Save $c $andesite "$blocks/cart_assembler_model.png"

# --- controller rail: golden rails with a direction arrow (pointing north = up in the texture) -----------
function Controller-Rail([switch]$On) {
    $c = New-Canvas 16 16
    foreach ($y in @(1, 5, 9, 13)) { Fill $c 1 $y 14 ($y + 1) 'w' ; Fill $c 1 ($y + 1) 14 ($y + 1) 'v' }
    Fill $c 2 0 3 15 'G'; Fill $c 12 0 13 15 'G'; Fill $c 3 0 3 15 'Y'; Fill $c 13 0 13 15 'Y'
    $arrow = if ($On) { 'R' } else { 'x' }
    Set-Px $c 7 4 $arrow; Set-Px $c 8 4 $arrow
    Fill $c 6 5 9 5 $arrow; Fill $c 5 6 10 6 $arrow; Fill $c 7 7 8 11 $arrow
    if ($On) { Set-Px $c 7 3 'Q'; Set-Px $c 8 3 'Q' }
    return , $c
}
$railPal = @{ w = '#7a5a34'; v = '#5a4024'; G = '#c9a227'; Y = '#f2d04a'; x = '#6b1d14'; R = '#e2311c'; Q = '#ff8a6a' }
Save (Controller-Rail) $railPal "$blocks/controller_rail.png"
Save (Controller-Rail -On) $railPal "$blocks/controller_rail_on.png"

# --- sails ---------------------------------------------------------------------------------------------------
function Sail-Frame($c) {
    Fill $c 0 0 15 0 'F'; Fill $c 0 15 15 15 'F'; Fill $c 0 0 0 15 'F'; Fill $c 15 0 15 15 'F'
    Fill $c 1 1 14 1 'D'; Fill $c 1 1 1 14 'D'
    for ($i = 1; $i -le 14; $i++) { Set-Px $c $i $i 'F'; Set-Px $c (15 - $i) $i 'F' }
}
$c = New-Canvas 16 16; Sail-Frame $c
Save $c @{ F = '#9c7446'; D = '#6f4f2c' } "$blocks/sail_frame.png"
$colours = [ordered]@{
    white = '#e9ecec'; orange = '#f07613'; magenta = '#bd44b3'; light_blue = '#3aafd9'; yellow = '#f8c527'; lime = '#70b919'
    pink = '#ed8dac'; gray = '#3e4447'; light_gray = '#8e8e86'; cyan = '#158991'; purple = '#792aac'; blue = '#35399d'
    brown = '#724728'; green = '#546d1b'; red = '#a12722'; black = '#141519'
}
foreach ($name in $colours.Keys) {
    $base = $colours[$name]
    $c = New-Canvas 16 16
    for ($y = 1; $y -le 14; $y++) { for ($x = 1; $x -le 14; $x++) {
        Set-Px $c $x $y ($(if ((($x + $y * 3) % 9) -eq 0) { 'b' } elseif ($y % 5 -eq 0) { 's' } else { 'a' }))
    } }
    Fill $c 0 0 15 0 'F'; Fill $c 0 15 15 15 'F'; Fill $c 0 0 0 15 'F'; Fill $c 15 0 15 15 'F'
    Set-Px $c 1 1 'D'; Set-Px $c 14 1 'D'; Set-Px $c 1 14 'D'; Set-Px $c 14 14 'D'
    Save $c @{ a = $base; b = (Shade-Color $base 0.88); s = (Shade-Color $base 0.8); F = '#9c7446'; D = '#6f4f2c' } "$blocks/${name}_sail.png"
}

# --- items -----------------------------------------------------------------------------------------------------
$coupling = @(
    '................',
    '..........kkk...',
    '.........kAAAk..',
    '........kAk.kAk.',
    '........kAk.kAk.',
    '.........kAAAk..',
    '.......kkSkkk...',
    '......kSSk......',
    '.....kSSk.......',
    '...kkkSk........',
    '..kAAAk.........',
    '.kAk.kAk........',
    '.kAk.kAk........',
    '..kAAAk.........',
    '...kkk..........',
    '................'
)
Save-PixelArt -Rows $coupling -Palette @{ k = '#2e2e2e'; A = '#a49e8a'; S = '#c8c8c8' } -Path "$items/minecart_coupling.png"
$contraption = @(
    '................',
    '.....pppppp.....',
    '....pllllllp....',
    '....pllllllp....',
    '....pldddllp....',
    '....pppppppp....',
    '..kkkkkkkkkkkk..',
    '..kmmmmmmmmmmk..',
    '..kmmmmmmmmmmk..',
    '..kmnnnnnnnnmk..',
    '...kmmmmmmmmk...',
    '...kkkkkkkkkk...',
    '...kwk....kwk...',
    '....k......k....',
    '................',
    '................'
)
Save-PixelArt -Rows $contraption -Palette @{ k = '#2b2b2b'; m = '#7d7d7d'; n = '#a0a0a0'; w = '#4a4a4a'; p = '#6f4f2c'; l = '#c09560'; d = '#a47b4a' } -Path "$items/minecart_contraption.png"
if ($Preview) {
    Save-PixelArt -Rows $coupling -Palette @{ k = '#2e2e2e'; A = '#a49e8a'; S = '#c8c8c8' } -Path (Join-Path $root 'tools/textures/preview/minecart_coupling.png') -Scale 8
}
Write-Host 'bearings textures written'
