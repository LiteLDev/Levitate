# Textures of the actors unit (U22): harvester, plough, roller, portable storage interface,
# contraption controls and the 16 seats. Original pixel art.
#
#   pwsh tools/textures/actors.ps1            # writes assets/pack/rp/textures/blocks/
#   pwsh tools/textures/actors.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$blocks = Join-Path $root 'assets/pack/rp/textures/blocks'

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

# --- andesite casing: grey stone in a wooden frame ----------------------------------------------
$casingPal = @{ a = '#a3a69e'; b = '#8b8e86'; c = '#73766f'; e = '#c4c7bf'; k = '#4c4e49'; w = '#9c7448'; v = '#7a5833'; h = '#b98d5a' }
function Casing($c) {
    for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        $k = if ((($x * 7 + $y * 5) % 9) -eq 0) { 'b' } elseif ((($x * 3 + $y * 11) % 13) -eq 0) { 'e' } else { 'a' }
        Set-Px $c $x $y $k
    } }
    Fill $c 0 0 15 1 'w'; Fill $c 0 14 15 15 'w'; Fill $c 0 0 1 15 'w'; Fill $c 14 0 15 15 'w'
    Fill $c 0 0 15 0 'h'; Fill $c 0 0 0 15 'h'; Fill $c 0 15 15 15 'v'; Fill $c 15 0 15 15 'v'
    foreach ($p in @(@(1, 1), @(14, 1), @(1, 14), @(14, 14))) { Set-Px $c $p[0] $p[1] 'k' }
    Fill $c 2 2 13 2 'c'; Fill $c 2 2 2 13 'c'
}
$c = New-Canvas; Casing $c; Save $c $casingPal "$blocks/actor_casing.png"

# --- harvester blades: polished iron teeth with dark gaps ---------------------------------------
$bladePal = @{ s = '#d8dde0'; m = '#b4bcc2'; d = '#7f878d'; k = '#3f4448'; e = '#f4f7f8' }
$c = New-Canvas
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
    $k = if ($x % 4 -eq 3) { 'k' } elseif ($x % 4 -eq 0) { 'e' } elseif (($y + $x) % 5 -eq 0) { 'd' } elseif ($x % 4 -eq 2) { 'm' } else { 's' }
    Set-Px $c $x $y $k
} }
Fill $c 0 0 15 0 'd'
Save $c $bladePal "$blocks/harvester_blade.png"

# --- plough share: dark iron with a bright cutting edge -----------------------------------------
$ploughPal = @{ s = '#8d969c'; m = '#737b81'; d = '#575e63'; e = '#dfe5e8'; k = '#34383b' }
$c = New-Canvas
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
    $k = if ($y -ge 13) { 'e' } elseif ((($x * 5 + $y * 3) % 7) -eq 0) { 'd' } elseif ($y % 5 -eq 0) { 'm' } else { 's' }
    Set-Px $c $x $y $k
} }
Fill $c 0 12 15 12 'k'
Save $c $ploughPal "$blocks/plough_blade.png"

# --- roller drum: heavy grey drum with ribs -----------------------------------------------------
$drumPal = @{ a = '#6f7378'; b = '#5b5f63'; r = '#8f9499'; k = '#2f3133'; e = '#a9aeb3' }
$c = New-Canvas
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
    $k = if ($y % 4 -eq 0) { 'r' } elseif ($y % 4 -eq 1) { 'e' } elseif ($y % 4 -eq 3) { 'k' } elseif (($x * 3 + $y) % 7 -eq 0) { 'b' } else { 'a' }
    Set-Px $c $x $y $k
} }
Save $c $drumPal "$blocks/roller_drum.png"

# --- portable storage interface: casing with a brass ring and a dark hatch ----------------------
function Merge-Pal($base, $over) { $h = @{}; foreach ($k in $base.Keys) { $h[$k] = $base[$k] }; foreach ($k in $over.Keys) { $h[$k] = $over[$k] }; $h }
$psiPal = Merge-Pal $casingPal @{ y = '#d6a33c'; n = '#a87a23'; o = '#f1cd6a'; t = '#3b3632'; g = '#5a5148' }
$c = New-Canvas; Casing $c; Fill $c 4 4 11 11 'n'; Fill $c 5 5 10 10 'y'; Fill $c 5 5 10 5 'o'; Fill $c 6 6 9 9 'c'
Save $c $psiPal "$blocks/psi_side.png"
$c = New-Canvas; Casing $c; Fill $c 2 2 13 13 'n'; Fill $c 3 3 12 12 't'; Fill $c 3 3 12 3 'g'
foreach ($i in 4..11) { if ($i % 2 -eq 0) { Fill $c $i 5 $i 11 'g' } }
Save $c $psiPal "$blocks/psi_front.png"
$c = New-Canvas
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { Set-Px $c $x $y ($(if ((($x + $y) % 6) -eq 0) { 'o' } elseif ($y % 5 -eq 4) { 'n' } else { 'y' })) } }
Fill $c 0 0 15 0 'o'; Fill $c 0 15 15 15 'n'
Save $c $psiPal "$blocks/psi_plate.png"

# --- contraption controls: dark panel, red push button, indicator lamps -------------------------
$ctrlPal = @{ p = '#3a3d42'; q = '#2c2e32'; l = '#52565c'; r = '#c0392b'; s = '#8e2a20'; w = '#ea6b5e'; g = '#58d05a'; h = '#2f8f33'; o = '#f0a03a'; u = '#a8651c'; y = '#d6a33c' }
$c = New-Canvas
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { Set-Px $c $x $y ($(if ((($x * 3 + $y * 7) % 11) -eq 0) { 'q' } else { 'p' })) } }
Fill $c 0 0 15 0 'l'; Fill $c 0 0 0 15 'l'; Fill $c 1 13 14 14 'y'
Save $c $ctrlPal "$blocks/controls_panel.png"
$c = New-Canvas 'r'; Fill $c 0 12 15 15 's'; Fill $c 0 0 15 1 'w'; Fill $c 0 0 1 11 'w'; Fill $c 14 0 15 15 's'
Save $c $ctrlPal "$blocks/controls_button.png"
$c = New-Canvas 'g'; Fill $c 0 8 15 15 'h'
Save $c $ctrlPal "$blocks/controls_indicator_on.png"
$c = New-Canvas 'o'; Fill $c 0 8 15 15 'u'
Save $c $ctrlPal "$blocks/controls_indicator_off.png"

# --- seats: wooden base, coloured cushion ---------------------------------------------------------
$basePal = @{ p = '#a47b4a'; l = '#b88f5a'; d = '#7c5a33'; g = '#6b4c2a' }
$c = New-Canvas
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
    $k = if ($y % 4 -eq 3) { 'd' } elseif ((($x + [math]::Floor($y / 4) * 5) % 16) -eq 0) { 'g' } elseif (($x * 7 + $y * 3) % 11 -eq 0) { 'l' } else { 'p' }
    Set-Px $c $x $y $k
} }
Save $c $basePal "$blocks/seat_base.png"

$wool = [ordered]@{
    white = '#e9ecec'; orange = '#f07613'; magenta = '#bd44b3'; light_blue = '#3aafd9'; yellow = '#f8c627'; lime = '#70b919'
    pink = '#ed8dac'; gray = '#3e4447'; light_gray = '#8e8e86'; cyan = '#158991'; purple = '#792aac'; blue = '#35399d'
    brown = '#724728'; green = '#546d1b'; red = '#a12722'; black = '#141519'
}
foreach ($name in $wool.Keys) {
    $base = $wool[$name]
    $pal = @{ a = $base; b = (Shade-Color $base 0.88); c = (Shade-Color $base 0.74); e = (Shade-Color $base 1.12); p = $basePal.p; d = $basePal.d; g = $basePal.g }
    # top: cushion with a quilted seam and a raised border
    $c = New-Canvas
    for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        $k = if ((($x * 5 + $y * 3) % 7) -eq 0) { 'b' } elseif ((($x + $y * 7) % 13) -eq 0) { 'e' } else { 'a' }
        Set-Px $c $x $y $k
    } }
    Fill $c 0 0 15 0 'c'; Fill $c 0 15 15 15 'c'; Fill $c 0 0 0 15 'c'; Fill $c 15 0 15 15 'c'
    Fill $c 1 1 14 1 'e'; Fill $c 1 1 1 14 'e'
    foreach ($i in 3..12) { Set-Px $c $i 8 'b'; Set-Px $c 8 $i 'b' }
    foreach ($p in @(@(4, 4), @(11, 4), @(4, 11), @(11, 11))) { Set-Px $c $p[0] $p[1] 'c' }
    Save $c $pal "$blocks/${name}_seat_top.png"
    # side: cushion (upper rows, y 8..12 of the block) over the wooden base (y 13..15)
    $c = New-Canvas
    for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        $k = if ($y -ge 13) { $(if ($y -eq 15) { 'd' } elseif (($x * 7) % 11 -eq 0) { 'g' } else { 'p' }) } elseif ($y -eq 12) { 'c' } elseif ($y -eq 8) { 'e' } elseif ((($x * 5 + $y * 3) % 7) -eq 0) { 'b' } else { 'a' }
        Set-Px $c $x $y $k
    } }
    Save $c $pal "$blocks/${name}_seat_side.png"
}
Write-Host 'actors textures written'
