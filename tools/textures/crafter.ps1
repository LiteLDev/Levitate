# Textures of the crafter unit (U16): mechanical crafter, mechanical saw, mechanical drill.
# Original pixel art.
#
#   pwsh tools/textures/crafter.ps1            # writes assets/pack/rp/textures/blocks/
#   pwsh tools/textures/crafter.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$blocks = Join-Path $root 'assets/pack/rp/textures/blocks'

# --- canvas helpers (w x h char grids) ---------------------------------------------------------------
function New-Canvas([int]$w = 16, [int]$h = 16, [char]$fill = '.') { , (0..($h - 1) | ForEach-Object { , ([char[]]((New-Object string $fill, $w))) }) }
function Set-Px($c, $x, $y, [char]$k) { if ($y -ge 0 -and $y -lt $c.Count -and $x -ge 0 -and $x -lt $c[0].Count) { $c[$y][$x] = $k } }
function Fill($c, $x0, $y0, $x1, $y1, [char]$k) { for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y $k } } }
function Rows($c) { , ($c | ForEach-Object { -join $_ }) }
function Save($c, $pal, $name) {
    $rows = Rows $c
    Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $blocks "$name.png")
    if ($Preview) { Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $root "tools/textures/preview/$name.png") -Scale 8 }
}
# copies a 16x16 canvas into a bigger one at (ox, oy)
function Blit($dst, $src, $ox, $oy) { for ($y = 0; $y -lt $src.Count; $y++) { for ($x = 0; $x -lt $src[0].Count; $x++) { Set-Px $dst ($x + $ox) ($y + $oy) $src[$y][$x] } } }

# A casing face: bevelled frame (H light, S shadow, F frame) around a plate (p) with rivets (n).
function Casing($c, [int]$ox = 0, [int]$oy = 0, [int]$w = 16, [int]$h = 16) {
    Fill $c $ox $oy ($ox + $w - 1) ($oy + $h - 1) 'p'
    for ($y = $oy + 2; $y -lt $oy + $h - 2; $y++) { for ($x = $ox + 2; $x -lt $ox + $w - 2; $x++) { if ((($x * 5 + $y * 3) % 13) -eq 0) { Set-Px $c $x $y 'q' } } }
    Fill $c $ox $oy ($ox + $w - 1) ($oy + 1) 'F'; Fill $c $ox ($oy + $h - 2) ($ox + $w - 1) ($oy + $h - 1) 'F'
    Fill $c $ox $oy ($ox + 1) ($oy + $h - 1) 'F'; Fill $c ($ox + $w - 2) $oy ($ox + $w - 1) ($oy + $h - 1) 'F'
    Fill $c $ox $oy ($ox + $w - 1) $oy 'H'; Fill $c $ox $oy $ox ($oy + $h - 1) 'H'
    Fill $c $ox ($oy + $h - 1) ($ox + $w - 1) ($oy + $h - 1) 'S'; Fill $c ($ox + $w - 1) $oy ($ox + $w - 1) ($oy + $h - 1) 'S'
    foreach ($p in @(@(2, 2), @(($w - 3), 2), @(2, ($h - 3)), @(($w - 3), ($h - 3)))) { Set-Px $c ($ox + $p[0]) ($oy + $p[1]) 'n' }
}
# a round shaft hole with a cross axle (the kinetic input)
function Shaft-Hole($c, [int]$cx = 8, [int]$cy = 8) {
    for ($y = $cy - 4; $y -le $cy + 3; $y++) { for ($x = $cx - 4; $x -le $cx + 3; $x++) {
        $dx = $x - ($cx - .5); $dy = $y - ($cy - .5); $r = [math]::Sqrt($dx * $dx + $dy * $dy)
        if ($r -le 3.6) { Set-Px $c $x $y 'o' }
        if ($r -le 3.6 -and $r -gt 2.8) { Set-Px $c $x $y 'S' }
    } }
    Fill $c ($cx - 1) ($cy - 3) $cx ($cy + 2) 'a'; Fill $c ($cx - 3) ($cy - 1) ($cx + 2) $cy 'a'
}

# --- mechanical crafter ---------------------------------------------------------------------------------
$brass = @{ p = '#b8893b'; q = '#a3762e'; F = '#7a5a24'; H = '#e2bd6b'; S = '#5e4317'; n = '#4a3410'; o = '#2a2118'; a = '#9a9d96'
            g = '#3a3a36'; e = '#56564f'; r = '#e9e3c9'; b = '#9a8f6f'; c = '#c0c3bb'; d = '#7d807a' }

$side = New-Canvas; Casing $side
# gear teeth peeking through a window
Fill $side 4 6 11 9 'g'; foreach ($x in 4, 6, 8, 10) { Fill $side $x 6 $x 7 'e' }
Save $side $brass 'mechanical_crafter_side'

$back = New-Canvas; Casing $back; Shaft-Hole $back
Save $back $brass 'mechanical_crafter_back'

# front: the slot (dark square) with the item-flow arrow on the frame
function Crafter-Front([string]$dir, [switch]$Covered) {
    $c = New-Canvas; Casing $c
    Fill $c 4 4 11 11 'S'; Fill $c 5 5 10 10 'g'
    if ($Covered) { Fill $c 4 4 11 11 'd'; Fill $c 5 5 10 10 'c'; Fill $c 5 5 10 5 'r'; Fill $c 6 7 9 8 'd' }
    # arrow on the frame (2 px wide band at the edge of the target side)
    switch ($dir) {
        'up' { Set-Px $c 7 0 'r'; Set-Px $c 8 0 'r'; Fill $c 6 1 9 1 'r'; Fill $c 5 2 10 2 'b' }
        'down' { Set-Px $c 7 15 'r'; Set-Px $c 8 15 'r'; Fill $c 6 14 9 14 'r'; Fill $c 5 13 10 13 'b' }
        'left' { Set-Px $c 0 7 'r'; Set-Px $c 0 8 'r'; Fill $c 1 6 1 9 'r'; Fill $c 2 5 2 10 'b' }
        'right' { Set-Px $c 15 7 'r'; Set-Px $c 15 8 'r'; Fill $c 14 6 14 9 'r'; Fill $c 13 5 13 10 'b' }
    }
    $c
}
foreach ($d in 'up', 'left', 'down', 'right') {
    Save (Crafter-Front $d) $brass "mechanical_crafter_front_$d"
    Save (Crafter-Front $d -Covered) $brass "mechanical_crafter_front_${d}_covered"
}

# --- saw and drill (32x32 atlases used by their geometry) ---------------------------------------------
$andesite = @{ p = '#9da39a'; q = '#8b918a'; F = '#6e736b'; H = '#c4c9c0'; S = '#4e524c'; n = '#3a3d38'; o = '#26241f'; a = '#b48a52'
               w = '#7a5a34'; y = '#a47b4a'; t = '#d8dcd8'; m = '#9fa6a8'; u = '#6f777a'; k = '#4c4f50'; j = '#e8ecee' }

# saw: (0,0) casing side, (16,0) top with the blade slot, (0,16) blade, (16,16) back
$saw = New-Canvas 32 32
Casing $saw 0 0 16 16; Fill $saw 2 0 13 1 'w'; Fill $saw 2 0 13 0 'y' # wooden trim at the blade side
Casing $saw 16 0 16 16; Fill $saw 22 2 25 13 'o'; Fill $saw 23 2 24 13 'k'
# blade: a toothed disc
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
    $dx = $x - 7.5; $dy = $y - 7.5; $r = [math]::Sqrt($dx * $dx + $dy * $dy); $ang = [math]::Atan2($dy, $dx)
    $tooth = ([math]::Floor(($ang + [math]::PI) / (2 * [math]::PI) * 12) % 2) -eq 0
    if ($r -le 6.2) { Set-Px $saw $x ($y + 16) ($(if ($r -le 1.6) { 'k' } elseif ($r -le 4.8) { 'm' } else { 't' })) }
    elseif ($r -le 7.8 -and $tooth) { Set-Px $saw $x ($y + 16) 'j' }
    if ($r -le 6.2 -and $r -gt 5.4) { Set-Px $saw $x ($y + 16) 'u' }
} }
Casing $saw 16 16 16 16
$sb = New-Canvas; Shaft-Hole $sb
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { if ($sb[$y][$x] -ne '.') { Set-Px $saw ($x + 16) ($y + 16) $sb[$y][$x] } } }
Save $saw $andesite 'mechanical_saw'

# drill: (0,0) casing side, (16,0) back with the shaft, (0,16) drill head, (16,16) casing front
$drill = New-Canvas 32 32
Casing $drill 0 0 16 16
Casing $drill 16 0 16 16
$db = New-Canvas; Shaft-Hole $db
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { if ($db[$y][$x] -ne '.') { Set-Px $drill ($x + 16) $y $db[$y][$x] } } }
# drill head: spiral stripes of steel
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
    $k = if ((($x + $y) % 6) -lt 2) { 'u' } elseif ((($x + $y) % 6) -lt 4) { 'm' } else { 't' }
    Set-Px $drill $x ($y + 16) $k
} }
Casing $drill 16 16 16 16; Fill $drill 19 19 28 28 'S'; Fill $drill 20 20 27 27 'k'
Save $drill $andesite 'mechanical_drill'

Write-Host 'crafter textures written'
