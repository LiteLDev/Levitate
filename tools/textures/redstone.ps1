# Textures of the redstone unit (U23): redstone link, analog lever, brass diodes, latches, smart
# observer, threshold switch, redstone contact. Original pixel art.
#
#   pwsh tools/textures/redstone.ps1            # writes assets/pack/rp/textures/blocks/
#   pwsh tools/textures/redstone.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
#
# Atlases (match the geometry in assets/pack/rp/models/blocks/*.geo.json):
#   redstone_link 32x32: plate top (0,0), plate side (16,0), slot pads (0,16), antennas at
#     (16,16) transmitter off, (20,16) on, (24,16) receiver off, (28,16) on.
#   analog_lever 32x32: casing (0,0), handle (16,0), tip (20,0), dial (0,16).
#   diodes / latches 64x32: tops with the output pointing north / east / south / west at
#     u = 0 / 16 / 32 / 48, side (0,16), parts at (16,16): torch off, torch on, lamp off, lamp on,
#     lever off, lever on (4 px columns).
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$blocks = Join-Path $root 'assets/pack/rp/textures/blocks'
New-Item -ItemType Directory -Force $blocks | Out-Null

# --- canvas helpers ------------------------------------------------------------------------------
function New-Grid([int]$w, [int]$h, [char]$fill = '.') { , (0..($h - 1) | ForEach-Object { , ([char[]]((New-Object string $fill, $w))) }) }
function Px($g, [int]$x, [int]$y, [char]$k) { if ($y -ge 0 -and $y -lt $g.Count -and $x -ge 0 -and $x -lt $g[0].Count) { $g[$y][$x] = $k } }
function Fill($g, [int]$x0, [int]$y0, [int]$x1, [int]$y1, [char]$k) { for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { Px $g $x $y $k } } }
function Box($g, [int]$x0, [int]$y0, [int]$x1, [int]$y1, [char]$light, [char]$dark) {
    for ($x = $x0; $x -le $x1; $x++) { Px $g $x $y0 $light; Px $g $x $y1 $dark }
    for ($y = $y0; $y -le $y1; $y++) { Px $g $x0 $y $light; Px $g $x1 $y $dark }
}
function Blit($dst, $src, [int]$ox, [int]$oy) { for ($y = 0; $y -lt $src.Count; $y++) { for ($x = 0; $x -lt $src[0].Count; $x++) { if ($src[$y][$x] -ne '.') { Px $dst ($ox + $x) ($oy + $y) $src[$y][$x] } } } }
function Rotate-CW($g) {
    $n = $g.Count; $r = New-Grid $n $n
    for ($y = 0; $y -lt $n; $y++) { for ($x = 0; $x -lt $n; $x++) { $r[$y][$x] = $g[$n - 1 - $x][$y] } }
    , $r
}
function Rows($g) { , ($g | ForEach-Object { -join $_ }) }
function Save($g, $pal, [string]$name) {
    $rows = Rows $g
    Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $blocks "$name.png")
    if ($Preview) { Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $root "tools/textures/preview/$name.png") -Scale 8 }
}

$pal = @{
    # stone slab
    s = '#a3a3a0'; '2' = '#8a8a86'; l = '#bdbdb9'; k = '#5f5f5b'
    # andesite casing / frame
    a = '#9a9d96'; '3' = '#7d807a'; h = '#c3c5be'; d = '#55574f'
    # brass
    b = '#d9a83f'; '4' = '#a6762b'; y = '#f4d77c'
    # redstone
    r = '#6e1410'; '5' = '#e8321c'; g = '#ff9a6e'
    # wood
    w = '#93623a'; '6' = '#6b4526'
    # amethyst, blue, dark
    m = '#9a6ad6'; u = '#3f6fb5'; '7' = '#8fb8f0'; n = '#2d2b28'; x = '#3a3634'
    # observer / switch body
    o = '#6d4a36'; '8' = '#55392a'; p = '#8a6247'; e = '#b4b8b0'; '9' = '#4f5250'
}

# --- shared tiles --------------------------------------------------------------------------------
function Stone-Top {
    $g = New-Grid 16 16 's'
    for ($i = 0; $i -lt 16; $i++) { for ($j = 0; $j -lt 16; $j++) { if ((($i * 7 + $j * 5) % 13) -eq 0) { Px $g $i $j '2' } elseif ((($i * 3 + $j * 11) % 17) -eq 0) { Px $g $i $j 'l' } } }
    Box $g 0 0 15 15 'l' 'k'
    , $g
}
function Casing {
    $g = New-Grid 16 16 'a'
    for ($i = 0; $i -lt 16; $i++) { for ($j = 0; $j -lt 16; $j++) { if ((($i * 5 + $j * 3) % 11) -eq 0) { Px $g $i $j '3' } } }
    Box $g 0 0 15 15 'h' 'd'; Box $g 1 1 14 14 '3' '3'
    foreach ($c in @(@(2, 2), @(13, 2), @(2, 13), @(13, 13))) { Px $g $c[0] $c[1] 'd' }
    , $g
}

# --- redstone link -------------------------------------------------------------------------------
$link = New-Grid 32 32
$top = Casing
# brass frames around the two frequency slots (Create: z 5.5 and 10.5 on an upward link)
Box $top 5 3 10 7 'y' '4'; Box $top 5 8 10 12 'y' '4'
Fill $top 6 4 9 6 'n'; Fill $top 6 9 9 11 'n'
Blit $link $top 0 0
$side = Casing
Fill $side 0 0 15 1 'b'; Fill $side 0 14 15 15 '4'
Blit $link $side 16 0
Fill $link 0 16 7 23 'b'; Box $link 0 16 7 23 'y' '4'
Fill $link 16 16 19 23 'r'; Fill $link 16 16 19 17 'x'      # transmitter antenna off
Fill $link 20 16 23 23 '5'; Fill $link 20 16 23 17 'g'      # transmitter on
Fill $link 24 16 27 23 '9'; Fill $link 24 16 27 17 'u'      # receiver off
Fill $link 28 16 31 23 '5'; Fill $link 28 16 31 17 '7'      # receiver on
Save $link $pal 'redstone_link'

# --- analog lever --------------------------------------------------------------------------------
$lever = New-Grid 32 32
Blit $lever (Casing) 0 0
Fill $lever 16 0 17 11 'w'; Fill $lever 17 0 17 11 '6'
Fill $lever 20 0 23 3 '5'; Px $lever 20 0 'g'
Fill $lever 0 16 7 23 'n'
for ($i = 0; $i -lt 6; $i++) { Px $lever (1 + $i) (21 - [math]::Floor($i / 2)) '5' }
Save $lever $pal 'analog_lever'

# --- brass diodes and latches --------------------------------------------------------------------
function Diode-Atlas([string]$name, [scriptblock]$decorate) {
    $base = Stone-Top
    & $decorate $base
    $atlas = New-Grid 64 32
    $t = $base
    foreach ($u in 0, 16, 32, 48) { Blit $atlas $t $u 0; $t = Rotate-CW $t }
    $side = New-Grid 16 16 's'
    Fill $side 0 14 15 15 '2'; Fill $side 0 14 15 14 'l'
    Blit $atlas $side 0 16
    # parts: torch off / on, lamp off / on, lever off / on
    Fill $atlas 16 16 19 23 '6'; Fill $atlas 16 16 19 17 'r'
    Fill $atlas 20 16 23 23 '6'; Fill $atlas 20 16 23 17 '5'; Px $atlas 20 16 'g'
    Fill $atlas 24 16 27 19 'r'
    Fill $atlas 28 16 31 19 '5'; Px $atlas 28 16 'g'
    Fill $atlas 32 16 35 23 '3'; Fill $atlas 32 16 35 17 'd'
    Fill $atlas 36 16 39 23 '3'; Fill $atlas 36 16 39 17 '5'
    Save $atlas $pal $name
}
# the output is at the top (north) of the base art
$wireLine = { param($g) Fill $g 7 1 8 14 'r' }
Diode-Atlas 'pulse_repeater' {
    param($g)
    & $wireLine $g
    Box $g 5 5 10 10 'y' '4'; Fill $g 6 6 9 9 'b'
    Px $g 7 2 '5'; Px $g 8 2 '5'; Px $g 6 3 '5'; Px $g 9 3 '5'
}
Diode-Atlas 'pulse_extender' {
    param($g)
    & $wireLine $g
    Box $g 5 5 10 10 'y' '4'; Fill $g 6 6 9 9 'b'
    Fill $g 6 2 9 2 '5'; Fill $g 6 12 9 12 '5'; Fill $g 5 3 5 4 'r'; Fill $g 10 3 10 4 'r'
}
Diode-Atlas 'pulse_timer' {
    param($g)
    & $wireLine $g
    Box $g 4 4 11 11 'y' '4'; Fill $g 5 5 10 10 'b'
    Px $g 7 6 'n'; Px $g 7 7 'n'; Px $g 8 7 'n'; Px $g 7 12 'm'; Px $g 8 12 'm'; Px $g 7 13 'm'; Px $g 8 13 'm'
}
Diode-Atlas 'powered_latch' {
    param($g)
    & $wireLine $g
    Fill $g 1 7 14 8 'r'
    Box $g 5 4 10 11 'h' 'd'; Fill $g 6 5 9 10 '3'
}
Diode-Atlas 'powered_toggle_latch' {
    param($g)
    & $wireLine $g
    Box $g 5 4 10 11 'h' 'd'; Fill $g 6 5 9 10 '3'; Px $g 7 7 '5'; Px $g 8 8 '5'
}

# --- smart observer (content_observer) -----------------------------------------------------------
function Body {
    $g = New-Grid 16 16 'o'
    for ($i = 0; $i -lt 16; $i++) { for ($j = 0; $j -lt 16; $j++) { if ((($i * 3 + $j * 7) % 9) -eq 0) { Px $g $i $j '8' } } }
    Box $g 0 0 15 15 'b' '4'
    , $g
}
$front = Body; Fill $front 3 3 12 12 '9'; Box $front 3 3 12 12 'e' 'n'; Fill $front 5 6 10 9 'n'; Fill $front 6 7 9 8 'x'
Save $front $pal 'content_observer_front'
$frontOn = Body; Fill $frontOn 3 3 12 12 '9'; Box $frontOn 3 3 12 12 'e' 'n'; Fill $frontOn 5 6 10 9 'n'; Fill $frontOn 6 7 9 8 '5'; Px $frontOn 7 7 'g'
Save $frontOn $pal 'content_observer_front_powered'
$back = Body; Fill $back 6 6 9 9 'r'; Box $back 5 5 10 10 'y' '4'
Save $back $pal 'content_observer_back'
$sideO = Body; Fill $sideO 2 7 13 8 'p'
Save $sideO $pal 'content_observer_side'
$topO = Body; Fill $topO 7 2 8 13 'p'; Box $topO 4 4 11 11 'y' '4'
Save $topO $pal 'content_observer_top'

# --- threshold switch (stockpile_switch) ---------------------------------------------------------
$sf = Body; Fill $sf 3 3 12 12 '3'; Box $sf 3 3 12 12 'h' 'd'; Fill $sf 6 6 9 9 'n'
Save $sf $pal 'stockpile_switch_front'
$ss = Body; Fill $ss 2 2 13 13 'p'; Box $ss 2 2 13 13 'y' '4'
Save $ss $pal 'stockpile_switch_side'
$st = Body; Fill $st 7 2 8 13 'p'
Save $st $pal 'stockpile_switch_top'
for ($level = 0; $level -le 5; $level++) {
    foreach ($on in $false, $true) {
        $g = Body
        Fill $g 4 2 11 13 'n'; Box $g 3 1 12 14 'y' '4'
        for ($i = 0; $i -lt $level; $i++) { Fill $g 5 (11 - 2 * $i) 10 (11 - 2 * $i) 'b' }
        Fill $g 6 13 9 13 ($(if ($on) { '5' } else { 'r' }))
        Save $g $pal "stockpile_switch_back_${level}_$(if ($on) { 'on' } else { 'off' })"
    }
}

# --- redstone contact ----------------------------------------------------------------------------
function Contact-Face([char]$core) {
    $g = Casing
    Fill $g 4 4 11 11 'e'; Box $g 4 4 11 11 'h' '9'; Fill $g 6 6 9 9 $core
    , $g
}
Save (Contact-Face 'r') $pal 'redstone_contact_front'
Save (Contact-Face '5') $pal 'redstone_contact_front_powered'
$cb = Casing; Fill $cb 6 6 9 9 '3'
Save $cb $pal 'redstone_contact_back'
Save (Casing) $pal 'redstone_contact_side'
Write-Host "redstone textures written to $blocks"
