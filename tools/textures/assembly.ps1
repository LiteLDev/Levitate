# Textures of the assembly unit (U15): deployer and mechanical arm atlases (32x32, see the uv
# regions in assets/pack/rp/models/blocks/{deployer,mechanical_arm}*.geo.json). Original pixel art.
#
#   pwsh tools/textures/assembly.ps1            # writes assets/pack/rp/textures/blocks/
#   pwsh tools/textures/assembly.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$blocks = Join-Path $root 'assets/pack/rp/textures/blocks'

function New-Canvas([int]$n = 32) { , (0..($n - 1) | ForEach-Object { , ([char[]]((New-Object string '.', $n))) }) }
function Set-Px($c, $x, $y, [char]$k) { if ($y -ge 0 -and $y -lt $c.Count -and $x -ge 0 -and $x -lt $c[0].Count) { $c[$y][$x] = $k } }
function Fill($c, $x0, $y0, $x1, $y1, [char]$k) { for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y $k } } }
function Frame($c, $x0, $y0, $x1, $y1, [char]$hi, [char]$lo) {
    Fill $c $x0 $y0 $x1 $y0 $hi; Fill $c $x0 $y0 $x0 $y1 $hi
    Fill $c $x0 $y1 $x1 $y1 $lo; Fill $c $x1 $y0 $x1 $y1 $lo
}
# speckled fill: base key with a few lighter / darker pixels (deterministic)
function Speckle($c, $x0, $y0, $x1, $y1, [char]$base, [char]$light, [char]$dark) {
    for ($y = $y0; $y -le $y1; $y++) {
        for ($x = $x0; $x -le $x1; $x++) {
            $h = ($x * 7 + $y * 13 + $x * $y) % 17
            $k = if ($h -eq 0) { $light } elseif ($h -eq 5) { $dark } else { $base }
            Set-Px $c $x $y $k
        }
    }
}
function Save($c, $pal, $path) {
    $rows = , ($c | ForEach-Object { -join $_ })
    Save-PixelArt -Rows $rows[0] -Palette $pal -Path $path
    if ($Preview) { Save-PixelArt -Rows $rows[0] -Palette $pal -Path (Join-Path $root "tools/textures/preview/$(Split-Path -Leaf $path)") -Scale 8 }
}

$andesite = @{ a = '#a3a69d'; '1' = '#c3c6bc'; '2' = '#7b7e76'; '3' = '#5d5f58'; o = '#9c7148'; '4' = '#b88a5a'; w = '#74512f' }
$brass = @{ b = '#d9a43c'; '5' = '#f2cd6a'; y = '#a87622'; '6' = '#7d5414' }
$misc = @{ k = '#2b2a27'; s = '#8e9088'; '7' = '#b5b7ae'; d = '#4a4b46'; h = '#e3c9a0'; '8' = '#c4a57a'; r = '#6d4a2a' }
$pal = @{}; foreach ($t in $andesite, $brass, $misc) { foreach ($k in $t.Keys) { $pal[$k] = $t[$k] } }

# ---- deployer ---------------------------------------------------------------------------------
$c = New-Canvas
# casing side: andesite frame around planks
Speckle $c 0 0 15 15 'o' '4' 'w'
for ($y = 3; $y -le 12; $y += 3) { Fill $c 2 $y 13 $y 'w' }
Fill $c 0 0 15 1 'a'; Fill $c 0 14 15 15 'a'; Fill $c 0 0 1 15 'a'; Fill $c 14 0 15 15 'a'
Frame $c 0 0 15 15 '1' '3'
foreach ($p in @(@(2, 2), @(13, 2), @(2, 13), @(13, 13))) { Set-Px $c $p[0] $p[1] '2' }
# the band near the front (front bone sides use v 12..15)
Fill $c 0 12 15 15 '2'; Fill $c 0 12 15 12 'a'; Frame $c 0 12 15 15 'a' '3'
# front: dark plate with the hand opening
Speckle $c 16 0 31 15 '2' 'a' '3'
Frame $c 16 0 31 15 '1' '3'
Fill $c 20 4 27 11 'k'; Frame $c 19 3 28 12 '3' 'a'
foreach ($p in @(@(18, 2), @(29, 2), @(18, 13), @(29, 13))) { Set-Px $c $p[0] $p[1] 's' }
# shaft: side strip (horizontal 16x4 and vertical 4x16), end
Fill $c 0 16 15 19 's'; Fill $c 0 16 15 16 '7'; Fill $c 0 19 15 19 'd'
Fill $c 0 16 3 31 's'; Fill $c 0 16 0 31 '7'; Fill $c 3 16 3 31 'd'
for ($i = 2; $i -lt 16; $i += 4) { Set-Px $c $i 17 'd'; Set-Px $c 1 ($i + 16) 'd' }
Fill $c 4 20 7 23 's'; Frame $c 4 20 7 23 '7' 'd'; Set-Px $c 5 21 'k'; Set-Px $c 6 22 'k'
# hand (brass palm), fist, pole
Fill $c 16 16 21 17 'y'; Fill $c 16 18 21 23 'b'; Frame $c 16 18 21 23 '5' '6'; Fill $c 17 19 18 20 '5'
Fill $c 16 24 19 26 'y'; Fill $c 16 24 19 24 '5'
Fill $c 24 16 25 21 's'; Fill $c 24 16 24 21 '7'; Fill $c 26 16 27 17 '7'
Save $c $pal (Join-Path $blocks 'deployer.png')

# ---- mechanical arm ------------------------------------------------------------------------------
$c = New-Canvas
# base side (16x4) and top (16x16 at v 8)
Speckle $c 0 0 15 3 'a' '1' '2'; Fill $c 0 0 15 0 '1'; Fill $c 0 3 15 3 '3'
Speckle $c 0 8 15 23 'a' '1' '2'; Frame $c 0 8 15 23 '1' '3'
foreach ($p in @(@(2, 10), @(13, 10), @(2, 21), @(13, 21))) { Set-Px $c $p[0] $p[1] '3' }
# cog: wooden ring with teeth (10x10 top, 10x2 side)
for ($y = 0; $y -lt 10; $y++) { for ($x = 0; $x -lt 10; $x++) {
    $dx = $x - 4.5; $dy = $y - 4.5; $r = [math]::Sqrt($dx * $dx + $dy * $dy)
    $k = if ($r -le 1.6) { 's' } elseif ($r -le 3.6) { 'o' } elseif ($r -le 4.9 -and (([math]::Floor(([math]::Atan2($dy, $dx) + 3.15) * 8 / 3.1416)) % 2 -eq 0)) { 'w' } else { '.' }
    if ($k -ne '.') { Set-Px $c (16 + $x) $y $k }
} }
Fill $c 16 10 25 11 'w'; Fill $c 16 10 25 10 '4'
# turntable (8x8 at 16,12) in brass
Fill $c 16 12 23 19 'b'; Frame $c 16 12 23 19 '5' '6'; Set-Px $c 19 15 'y'; Set-Px $c 20 16 'y'
# arm segment strip (3 wide, 18 tall) and ends
Fill $c 28 0 30 17 'b'; Fill $c 28 0 28 17 '5'; Fill $c 30 0 30 17 '6'
for ($y = 2; $y -lt 18; $y += 5) { Set-Px $c 29 $y 'y' }
Fill $c 28 18 30 20 'y'; Set-Px $c 29 19 'k'
# claw head (6x6 at 16,22)
Fill $c 16 22 21 27 'd'; Frame $c 16 22 21 27 's' 'k'; Fill $c 18 24 19 25 'b'
Save $c $pal (Join-Path $blocks 'mechanical_arm.png')
Write-Host 'assembly textures written'
