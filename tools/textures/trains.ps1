# Textures of the trains unit (U27): train station, bogeys, train controls, schedule. Original
# pixel art.
#
#   pwsh tools/textures/trains.ps1            # writes assets/pack/rp/textures/{blocks,items}/
#   pwsh tools/textures/trains.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$blocks = Join-Path $root 'assets/pack/rp/textures/blocks'
$items = Join-Path $root 'assets/pack/rp/textures/items'

function New-Canvas([char]$fill = '.') { , (0..15 | ForEach-Object { , ([char[]]((New-Object string $fill, 16))) }) }
function Set-Px($c, $x, $y, [char]$k) { if ($x -ge 0 -and $x -lt 16 -and $y -ge 0 -and $y -lt 16) { $c[$y][$x] = $k } }
function Fill($c, $x0, $y0, $x1, $y1, [char]$k) { for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y $k } } }
function Save($c, $pal, $path) {
    $rows = @($c | ForEach-Object { -join $_ })
    Save-PixelArt -Rows $rows -Palette $pal -Path $path
    if ($Preview) { Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $root "tools/textures/preview/$(Split-Path -Leaf $path)") -Scale 8 }
}

# dark railway steel with a brass trim
$steel = @{ F = '#2f2b2a'; H = '#5b5450'; S = '#1d1a19'; p = '#433d3a'; l = '#4f4844'; d = '#38322f'; b = '#c49a4a'; m = '#8a6a2e'; r = '#b8403a'; g = '#58a04a'; w = '#e8e0d0'; k = '#151312' }

function Casing-Body($c) {
    for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        $k = if (($x + $y) % 7 -eq 0) { 'l' } elseif (($x * 3 + $y * 5) % 13 -eq 0) { 'd' } else { 'p' }
        Set-Px $c $x $y $k } }
    Fill $c 0 0 15 0 'H'; Fill $c 0 0 0 15 'H'; Fill $c 0 15 15 15 'S'; Fill $c 15 0 15 15 'S'
    foreach ($p in @(@(2, 2), @(13, 2), @(2, 13), @(13, 13))) { Set-Px $c $p[0] $p[1] 'b' }
}

# station side: a timetable board with a flag
function Station-Side($c, [switch]$Assembling) {
    Casing-Body $c
    Fill $c 3 3 12 10 'k'; Fill $c 3 3 12 3 'm'; Fill $c 3 10 12 10 'm'; Fill $c 3 3 3 10 'm'; Fill $c 12 3 12 10 'm'
    foreach ($y in 5, 7) { Fill $c 5 $y 10 $y 'w' }
    Set-Px $c 9 9 'w'; Set-Px $c 10 9 'w'
    $flag = if ($Assembling) { 'g' } else { 'r' }
    Fill $c 6 12 9 13 $flag
}
$c = New-Canvas; Station-Side $c; Save $c $steel (Join-Path $blocks 'track_station_side.png')
$c = New-Canvas; Station-Side $c -Assembling; Save $c $steel (Join-Path $blocks 'track_station_side_assembling.png')

$c = New-Canvas; Casing-Body $c
foreach ($y in 4..11) { foreach ($x in 4..11) { $dx = $x - 7.5; $dy = $y - 7.5; $r = [math]::Sqrt($dx * $dx + $dy * $dy); if ($r -le 3.9 -and $r -ge 2.4) { Set-Px $c $x $y 'b' } } }
Fill $c 7 5 8 8 'w'; Fill $c 8 8 10 8 'w'
Save $c $steel (Join-Path $blocks 'track_station_top.png')
$c = New-Canvas; Casing-Body $c
Save $c $steel (Join-Path $blocks 'track_station_bottom.png')

# bogey: frame plates with rivets and the wheel rim in the lower half
$c = New-Canvas; Casing-Body $c
Fill $c 0 6 15 7 'k'
foreach ($y in 8..15) { foreach ($x in 0..15) { $dx = $x - 7.5; $dy = $y - 11.5; $r = [math]::Sqrt($dx * $dx + $dy * $dy); if ($r -le 4.2) { Set-Px $c $x $y ($(if ($r -ge 3.2) { 'H' } elseif ($r -le 1.2) { 'b' } else { 'F' })) } } }
Save $c $steel (Join-Path $blocks 'bogey.png')

# controls: a dark desk with a brass lever and dials
$c = New-Canvas; Casing-Body $c
Fill $c 2 2 13 6 'k'
Set-Px $c 4 4 'g'; Set-Px $c 7 4 'r'; Set-Px $c 10 4 'w'; Set-Px $c 11 4 'w'
Fill $c 7 8 8 14 'm'; Fill $c 6 7 9 8 'b'
Save $c $steel (Join-Path $blocks 'controls.png')

# schedule: a paper timetable on a dark clipboard
$paper = @{ k = '#2a2421'; c = '#4a3f38'; p = '#f0e8d4'; q = '#d2c7ad'; t = '#3c3c48'; r = '#b8403a'; b = '#c49a4a' }
$c = New-Canvas
Fill $c 3 1 12 15 'c'; Fill $c 3 1 3 15 'k'; Fill $c 12 1 12 15 'k'; Fill $c 3 15 12 15 'k'
Fill $c 4 2 11 14 'p'; Fill $c 11 2 11 14 'q'; Fill $c 4 14 11 14 'q'
Fill $c 6 0 9 1 'b'
foreach ($y in 4, 6, 8, 10, 12) { Fill $c 5 $y 6 $y 'r'; Fill $c 8 $y 10 $y 't' }
Save $c $paper (Join-Path $items 'schedule.png')
Write-Host 'trains textures written'
