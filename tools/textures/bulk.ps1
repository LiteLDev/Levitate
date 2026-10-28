# Textures of the bulk processing unit (U13): millstone, crushing wheel, encased fan, nozzle and the
# invisible crushing wheel controller. Original pixel art, drawn procedurally.
#
#   pwsh tools/textures/bulk.ps1            # writes assets/pack/rp/textures/blocks/
#   pwsh tools/textures/bulk.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
#
# Every block texture is a 32x32 sheet of four 16x16 quadrants (the models' UVs):
#   millstone       Q0 casing side   Q1 casing top    Q2 stone side    Q3 stone top
#   crushing_wheel  Q0 wheel face    Q1 rim           Q2 -             Q3 axle plate
#   encased_fan     Q0 casing side   Q1 back plate    Q2 grille bar    Q3 shaft (x 16..19) + blades (x 20..31)
#   nozzle          Q0 side mesh     Q1 top           Q2 rim
# (Q0 = x 0..15 y 0..15, Q1 = x 16..31 y 0..15, Q2 = x 0..15 y 16..31, Q3 = x 16..31 y 16..31)
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root   = (Resolve-Path "$PSScriptRoot/../..").Path
$blocks = Join-Path $root 'assets/pack/rp/textures/blocks'

function New-Canvas([int]$w, [int]$h) { , (0..($h - 1) | ForEach-Object { , ([char[]]((New-Object string '.', $w))) }) }
function Set-Px($c, $x, $y, [char]$k) { if ($y -ge 0 -and $y -lt $c.Count -and $x -ge 0 -and $x -lt $c[0].Count) { $c[$y][$x] = $k } }
function Fill($c, $x0, $y0, $x1, $y1, [char]$k) { for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y $k } } }
function Paint($c, $x0, $y0, $w, $h, [scriptblock]$f) {
    for ($y = 0; $y -lt $h; $y++) { for ($x = 0; $x -lt $w; $x++) { $k = & $f $x $y; if ($k) { Set-Px $c ($x0 + $x) ($y0 + $y) ([char]$k) } } }
}
function Save($c, $pal, $name) {
    $rows = , ($c | ForEach-Object { -join $_ })
    $path = Join-Path $blocks "$name.png"
    Save-PixelArt -Rows $rows[0] -Palette $pal -Path $path
    if ($Preview) { Save-PixelArt -Rows $rows[0] -Palette $pal -Path (Join-Path $root "tools/textures/preview/$name.png") -Scale 8 }
}
# deterministic noise in 0..99
function Noise([int]$x, [int]$y, [int]$seed = 0) { return [math]::Abs((($x * 73856093) -bxor ($y * 19349663) -bxor ($seed * 83492791)) % 100) }

# shared palette: andesite casing (stone frame around wood), stone, iron, dark
$pal = @{
    F = '#8c8f89'; H = '#b3b6b0'; S = '#5f625d'   # stone frame, highlight, shadow
    p = '#a27d4f'; l = '#b8925f'; d = '#7c5d36'   # planks, light, dark seam
    g = '#7d7f7b'; a = '#959792'; k = '#5b5d59'; b = '#474946'   # stone shades
    i = '#c9cbc8'; e = '#e4e5e2'; j = '#8d908c'   # iron
    o = '#2b2b2b'; q = '#3d3a36'                  # dark hole, dark wood
    w = '#d9d4c7'; v = '#bcb6a6'                  # wool
}

# an andesite casing face: 2 px stone frame, planks inside
function Casing($c, $x0, $y0) {
    Paint $c $x0 $y0 16 16 {
        param($x, $y)
        if ($x -lt 2 -or $y -lt 2 -or $x -gt 13 -or $y -gt 13) {
            if ($x -eq 0 -or $y -eq 0) { 'H' } elseif ($x -eq 15 -or $y -eq 15) { 'S' } else { 'F' }
        } elseif ($y % 4 -eq 1) { 'd' } elseif ((Noise $x $y 3) -lt 12) { 'l' } else { 'p' }
    }
    foreach ($p in @(@(1, 1), @(14, 1), @(1, 14), @(14, 14))) { Set-Px $c ($x0 + $p[0]) ($y0 + $p[1]) 'j' }
}

# ---- millstone ---------------------------------------------------------------------------------------------
$c = New-Canvas 32 32
Casing $c 0 0
Casing $c 16 0
Fill $c 20 4 27 11 'g'; Fill $c 21 5 26 10 'k'                       # casing top: opening for the stone
Paint $c 0 16 16 16 { param($x, $y) if ($y % 5 -eq 4) { 'k' } elseif ((Noise $x $y 7) -lt 15) { 'a' } else { 'g' } }
Paint $c 16 16 16 16 {
    param($x, $y)
    $dx = $x - 7.5; $dy = $y - 7.5; $r = [math]::Sqrt($dx * $dx + $dy * $dy)
    if ($r -lt 2.2) { 'o' }
    elseif ($r -lt 3.2) { 'b' }
    elseif ([math]::Abs($dx) -lt 0.6 -or [math]::Abs($dy) -lt 0.6 -or [math]::Abs([math]::Abs($dx) - [math]::Abs($dy)) -lt 0.8) { 'k' }
    elseif ((Noise $x $y 11) -lt 15) { 'a' } else { 'g' }
}
Save $c $pal 'millstone'

# ---- crushing wheel -----------------------------------------------------------------------------------------
$c = New-Canvas 32 32
Paint $c 0 0 16 16 {
    param($x, $y)
    $dx = $x - 7.5; $dy = $y - 7.5; $r = [math]::Sqrt($dx * $dx + $dy * $dy)
    if ($r -gt 6.5) { 'k' } elseif ($r -gt 5.5) { 'a' } elseif ((Noise $x $y 5) -lt 18) { 'a' } else { 'g' }
}
Paint $c 16 0 16 16 { param($x, $y) if ($x % 4 -eq 0) { 'b' } elseif ($x % 4 -eq 1) { 'a' } elseif ((Noise $x $y 9) -lt 20) { 'k' } else { 'g' } }
Paint $c 16 16 16 16 {
    param($x, $y)
    $dx = $x - 7.5; $dy = $y - 7.5; $r = [math]::Sqrt($dx * $dx + $dy * $dy)
    if ($r -lt 1.6) { 'o' } elseif ($r -lt 3.5) { 'i' } elseif ($r -lt 5) { 'j' } elseif ($x -eq 0 -or $y -eq 0) { 'H' } elseif ($x -eq 15 -or $y -eq 15) { 'S' } else { 'F' }
}
Save $c $pal 'crushing_wheel'

# ---- encased fan ---------------------------------------------------------------------------------------------
$c = New-Canvas 32 32
Casing $c 0 0
Casing $c 16 0
Fill $c 22 6 25 9 'o'                                                       # back plate: shaft hole
Paint $c 0 16 16 16 { param($x, $y) if ($y -lt 2) { if ($y -eq 0) { 'e' } else { 'j' } } else { $null } }   # grille bar
Paint $c 16 16 4 16 { param($x, $y) if ($x -eq 0) { 'j' } elseif ($x -eq 3) { 'S' } elseif ($y % 4 -eq 0) { 'H' } else { 'i' } }  # shaft
Paint $c 20 16 12 16 { param($x, $y) if ($y -lt 3) { if ($x -eq 0 -or $x -eq 11) { 'q' } else { 'l' } } elseif ($y -lt 6) { 'd' } else { $null } }  # blades
Save $c $pal 'encased_fan'

# ---- nozzle --------------------------------------------------------------------------------------------------
$c = New-Canvas 32 32
Paint $c 0 0 16 16 {
    param($x, $y)
    if ($y -lt 1 -or $y -gt 14) { 'F' }
    elseif (($x + $y) % 4 -eq 0 -or ($x - $y + 16) % 4 -eq 0) { 'j' }
    elseif ((Noise $x $y 13) -lt 25) { 'v' } else { 'w' }
}
Paint $c 16 0 16 16 {
    param($x, $y)
    $dx = $x - 7.5; $dy = $y - 7.5; $r = [math]::Sqrt($dx * $dx + $dy * $dy)
    if ($r -lt 4.2) { 'o' } elseif ($r -lt 5.4) { 'b' } elseif ($x -eq 0 -or $y -eq 0) { 'H' } elseif ($x -eq 15 -or $y -eq 15) { 'S' } else { 'F' }
}
Paint $c 0 16 16 16 { param($x, $y) if ($y -eq 0) { 'H' } elseif ($y -eq 1) { 'F' } else { 'S' } }
Save $c $pal 'nozzle'

# ---- crushing wheel controller: invisible --------------------------------------------------------------------
Save (New-Canvas 16 16) $pal 'crushing_wheel_controller'
