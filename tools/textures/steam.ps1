# Textures of the steam unit (U18): fluid tanks, creative fluid tanks, steam engine, steam whistle,
# powered shaft. Original pixel art.
#   pwsh tools/textures/steam.ps1
#
# Tank sides come in four vertical pieces (single / top / middle / bottom, Levitate's connected
# textures) with and without a window; the window glass is transparent so the client mod's fluid
# shows through.
. "$PSScriptRoot/lib.ps1"
$out = Join-Path $PSScriptRoot '../../assets/pack/rp/textures/blocks'

function New-Canvas([int]$w, [int]$h, [char]$fill = '.') {
    $c = New-Object 'char[][]' $h
    for ($y = 0; $y -lt $h; $y++) { $c[$y] = ([string]$fill * $w).ToCharArray() }
    return , $c
}
function Fill($c, [int]$x, [int]$y, [int]$w, [int]$h, [char]$ch) {
    for ($j = $y; $j -lt $y + $h; $j++) { for ($i = $x; $i -lt $x + $w; $i++) { $c[$j][$i] = $ch } }
}
function Px($c, [int]$x, [int]$y, [char]$ch) { $c[$y][$x] = $ch }
function Rows($c) { return , @($c | ForEach-Object { -join $_ }) }

# ---- tanks ------------------------------------------------------------------------------------------
function Tank-Side([bool]$top, [bool]$bottom, [bool]$window) {
    $c = New-Canvas 16 16 'p'
    # plate: horizontal seams every 4 px, darker lower edge
    for ($y = 0; $y -lt 16; $y++) { if ($y % 4 -eq 3) { Fill $c 0 $y 16 1 'q' } }
    # vertical frame strips
    Fill $c 0 0 2 16 'm'; Fill $c 14 0 2 16 'm'
    Fill $c 0 0 1 16 'l'; Fill $c 15 0 1 16 'd'
    if ($top) { Fill $c 0 0 16 2 'm'; Fill $c 0 0 16 1 'l'; Px $c 3 1 'r'; Px $c 12 1 'r' }
    if ($bottom) { Fill $c 0 14 16 2 'm'; Fill $c 0 15 16 1 'd'; Px $c 3 14 'r'; Px $c 12 14 'r' }
    foreach ($y in 4, 11) { Px $c 1 $y 'r'; Px $c 14 $y 'r' }
    if ($window) {
        $y0 = if ($top) { 3 } else { 0 }
        $y1 = if ($bottom) { 12 } else { 15 }
        Fill $c 3 $y0 10 ($y1 - $y0 + 1) 'k'
        Fill $c 4 $y0 8 ($y1 - $y0 + 1) '.'
        if ($top) { Fill $c 3 $y0 10 1 'k' }
        if ($bottom) { Fill $c 3 $y1 10 1 'k' }
        # glass glints
        for ($y = $y0 + 1; $y -lt $y1; $y++) { if (($y % 5) -eq 1) { Px $c 5 $y 'g'; Px $c 6 ($y - 1) 'g' } }
        Px $c 10 ([Math]::Min($y1 - 1, $y0 + 6)) 'g'
    }
    return Rows $c
}
function Tank-Cap([bool]$isTop) {
    $c = New-Canvas 16 16 'p'
    Fill $c 0 0 16 16 'm'
    Fill $c 2 2 12 12 'p'
    Fill $c 0 0 16 1 'l'; Fill $c 0 0 1 16 'l'; Fill $c 0 15 16 1 'd'; Fill $c 15 0 1 16 'd'
    foreach ($p in @(@(1, 1), @(14, 1), @(1, 14), @(14, 14))) { Px $c $p[0] $p[1] 'r' }
    if ($isTop) {
        Fill $c 5 5 6 6 'm'; Fill $c 6 6 4 4 'q'; Px $c 7 7 'r'; Px $c 8 8 'r'
    } else {
        for ($i = 3; $i -lt 13; $i += 3) { Fill $c $i 2 1 12 'q' }
    }
    return Rows $c
}
function Save-TankSet([string]$name, [hashtable]$pal) {
    $pieces = @{ single = @($true, $true); top = @($true, $false); middle = @($false, $false); bottom = @($false, $true) }
    foreach ($k in $pieces.Keys) {
        $t = $pieces[$k][0]; $b = $pieces[$k][1]
        Save-PixelArt -Rows (Tank-Side $t $b $false) -Palette $pal -Path "$out/${name}_side_$k.png"
        Save-PixelArt -Rows (Tank-Side $t $b $true) -Palette $pal -Path "$out/${name}_window_$k.png"
    }
    Save-PixelArt -Rows (Tank-Cap $true) -Palette $pal -Path "$out/${name}_top.png"
    Save-PixelArt -Rows (Tank-Cap $false) -Palette $pal -Path "$out/${name}_bottom.png"
}

$copper = @{
    l = '#e8a878'; m = '#c47c4c'; d = '#7e4426'; p = '#a5603a'; q = '#8c4f2e'; r = '#f6d0a4'
    k = '#5b3020'; g = '#c8e8f8'
}
$creative = @{
    l = '#d9a6e6'; m = '#a45cbc'; d = '#4f2463'; p = '#7d3f96'; q = '#66307c'; r = '#f2dafa'
    k = '#3a1648'; g = '#e8d8ff'
}
Save-TankSet 'fluid_tank' $copper
Save-TankSet 'creative_fluid_tank' $creative

# ---- steam engine (32x32, layout of models/blocks/steam_engine.geo.json) ---------------------------------
$c = New-Canvas 32 32 '.'
Fill $c 0 0 14 14 'm'; Fill $c 1 1 12 12 'p'                       # base plate top
foreach ($p in @(@(1, 1), @(12, 1), @(1, 12), @(12, 12))) { Px $c $p[0] $p[1] 'r' }
Fill $c 0 14 14 2 'd'; Fill $c 0 14 14 1 'm'                         # base plate side
Fill $c 14 0 10 9 'i'; Fill $c 14 0 10 1 'j'; Fill $c 14 8 10 1 'n'  # cylinder side (iron)
Fill $c 14 3 10 1 'm'; Fill $c 14 6 10 1 'm'                         # copper bands
Fill $c 16 0 1 9 'j'
Fill $c 14 9 10 10 'i'; Fill $c 16 11 6 6 'n'; Fill $c 17 12 4 4 'j' # cylinder top
Fill $c 0 16 12 2 'm'; Fill $c 0 17 12 1 'd'                         # head ring side
Fill $c 0 18 12 12 'm'; Fill $c 2 20 8 8 'p'; Fill $c 4 22 4 4 'n'   # head ring top
Fill $c 24 0 3 3 'j'; Fill $c 27 0 3 3 'n'                           # piston rod
Fill $c 14 19 13 1 'r'                                               # band
Save-PixelArt -Rows (Rows $c) -Palette (@{
        m = '#c47c4c'; p = '#a5603a'; d = '#7e4426'; r = '#f6d0a4'
        i = '#6e6e72'; j = '#a9a9ad'; n = '#3c3c40'
    }) -Path "$out/steam_engine.png"

# ---- steam whistle (32x32, layout of models/blocks/steam_whistle*.geo.json) --------------------------------
$c = New-Canvas 32 32 '.'
Fill $c 0 0 8 16 'b'                                                 # pipe side
for ($x = 0; $x -lt 8; $x++) { if ($x % 3 -eq 0) { Fill $c $x 0 1 16 'h' } }
Fill $c 0 0 8 1 'h'; Fill $c 0 15 8 1 's'; Fill $c 0 7 8 1 's'
Fill $c 8 0 8 8 'b'; Fill $c 9 1 6 6 's'; Fill $c 10 2 4 4 'o'       # pipe top (open)
Fill $c 8 8 6 4 'o'                                                  # mouth
Fill $c 16 0 7 1 'h'                                                 # cap side
Fill $c 16 1 7 7 'b'; Fill $c 17 2 5 5 'h'                           # cap top
Fill $c 16 8 6 3 'm'; Fill $c 16 8 6 1 'r'                           # bracket side
Fill $c 22 8 3 3 'd'                                                 # bracket end
Save-PixelArt -Rows (Rows $c) -Palette (@{
        b = '#d6a83a'; h = '#f2d27a'; s = '#9c7420'; o = '#2a2218'
        m = '#c47c4c'; r = '#e8a878'; d = '#7e4426'
    }) -Path "$out/steam_whistle.png"

# ---- powered shaft (16x16) -----------------------------------------------------------------------------
$c = New-Canvas 16 16 '.'
for ($y = 0; $y -lt 16; $y++) { Px $c 0 $y 'b'; Px $c 1 $y 'c'; Px $c 2 $y 'a'; Px $c 3 $y 'b' }
Fill $c 4 0 4 4 'e'; Fill $c 5 1 2 2 'o'
Fill $c 8 0 6 4 'm'; Fill $c 8 0 6 1 'r'; Fill $c 8 3 6 1 'd'          # collar side
Fill $c 8 4 6 6 'm'; Fill $c 10 6 2 2 'o'                            # collar end
Save-PixelArt -Rows (Rows $c) -Palette (@{
        b = '#6b706d'; c = '#a9aeab'; a = '#8a8f8c'; e = '#4a4e4c'; o = '#2f2f2f'
        m = '#c47c4c'; r = '#e8a878'; d = '#7e4426'
    }) -Path "$out/powered_shaft.png"

Write-Host "steam textures written to $out"
