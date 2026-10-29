# Textures of the transmission unit (U6): original pixel art.
#   pwsh tools/textures/transmission.ps1
#
# - transmission_rotor.png (64x64): every rotating part (models/blocks/transmission_rotors.geo.json
#   and the rotor bones of the block models):
#     shaft side x0..3 y0..15, shaft end x4..7 y0..3,
#     small cog hub top x8..19 y0..11, hub side x8..19 y12..15,
#     small tooth top x20..35 y0..1, tooth side x20..35 y2..5, tooth end x36..37 y0..3,
#     large cog hub top x0..19 y20..39, hub side x0..19 y40..43,
#     large tooth top x20..43 y20..22, tooth side x20..43 y23..26, tooth end x44..46 y20..23,
#     gauge needle x48..51 y0..7.
# - 16x16 block faces: casings (andesite / brass), shaft holes, cog slots, clutch / gearshift /
#   chain drive / sequenced gearshift / speed controller sides, gauge faces.
# - items/vertical_gearbox.png.
. "$PSScriptRoot/lib.ps1"
$blocks = Join-Path $PSScriptRoot '../../assets/pack/rp/textures/blocks'
$items  = Join-Path $PSScriptRoot '../../assets/pack/rp/textures/items'

function New-Grid([int]$w, [int]$h, [char]$fill) {
    $g = New-Object 'char[][]' $h
    for ($y = 0; $y -lt $h; $y++) { $g[$y] = ([string]$fill * $w).ToCharArray() }
    return , $g
}
function Set-Rect($g, [int]$x0, [int]$y0, [int]$x1, [int]$y1, [char]$c) {
    for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { $g[$y][$x] = $c } }
}
function Set-Frame($g, [int]$x0, [int]$y0, [int]$x1, [int]$y1, [char]$c) {
    for ($x = $x0; $x -le $x1; $x++) { $g[$y0][$x] = $c; $g[$y1][$x] = $c }
    for ($y = $y0; $y -le $y1; $y++) { $g[$y][$x0] = $c; $g[$y][$x1] = $c }
}
function Set-Pixel($g, [int]$x, [int]$y, [char]$c) { $g[$y][$x] = $c }
function Rows($g) { return , @($g | ForEach-Object { -join $_ }) }
# speckle: deterministic noise of `c` over cells currently `on`
function Add-Speckle($g, [char]$on, [char]$c, [int]$seed, [int]$every) {
    $h = $g.Count; $w = $g[0].Count
    for ($y = 0; $y -lt $h; $y++) {
        for ($x = 0; $x -lt $w; $x++) {
            $v = (($x * 7349 + $y * 3163 + $seed * 911) -bxor ($x * $y + $seed)) % $every
            if ($g[$y][$x] -eq $on -and $v -eq 0) { $g[$y][$x] = $c }
        }
    }
}

$pal = @{
    # andesite casing: wooden frame around an andesite plate
    w = '#9b7448'; x = '#6f5131'; v = '#4f3a24'
    a = '#a3a69a'; b = '#c1c4b8'; c = '#7c7f75'; d = '#55574f'
    # brass casing: brass frame around dark planks
    y = '#e2b84f'; z = '#a77a29'; q = '#f3dc8c'
    k = '#5c4229'; l = '#704f31'; m = '#3e2c1b'
    # metal / shaft
    s = '#b4b8b4'; t = '#858a86'; u = '#5d615e'; o = '#2b2b2b'
    # cog wood
    e = '#b5864f'; f = '#8e6638'; g = '#6c4b27'; h = '#4c341b'
    # signals
    r = '#d23b2b'; p = '#ff7a59'; i = '#3f8fd8'; j = '#7fd0ff'; n = '#3fb94a'
}

function Andesite-Casing {
    $g = New-Grid 16 16 'a'
    Add-Speckle $g 'a' 'b' 1 9
    Add-Speckle $g 'a' 'c' 2 11
    Set-Frame $g 0 0 15 15 'v'
    Set-Frame $g 1 1 14 14 'w'
    Set-Frame $g 2 2 13 13 'x'
    foreach ($p in @(@(1, 1), @(14, 1), @(1, 14), @(14, 14))) { Set-Pixel $g $p[0] $p[1] 'd' }
    return , $g
}
function Brass-Casing {
    $g = New-Grid 16 16 'k'
    for ($y = 2; $y -le 13; $y++) { if ($y % 4 -eq 1) { Set-Rect $g 2 $y 13 $y 'm' } }
    Add-Speckle $g 'k' 'l' 3 7
    Set-Frame $g 0 0 15 15 'z'
    Set-Frame $g 1 1 14 14 'y'
    Set-Rect $g 1 1 14 1 'q'
    Set-Frame $g 2 2 13 13 'z'
    foreach ($p in @(@(1, 1), @(14, 1), @(1, 14), @(14, 14))) { Set-Pixel $g $p[0] $p[1] 'z' }
    return , $g
}
function With-Hole($g, [char]$ring) {
    Set-Rect $g 5 5 10 10 $ring
    Set-Rect $g 6 6 9 9 '.'
    return , $g
}
function With-Slot($g, [char]$edge) {
    # a band across the middle where the cog teeth show (rows 6..9)
    Set-Rect $g 0 5 15 5 $edge
    Set-Rect $g 0 10 15 10 $edge
    Set-Rect $g 0 6 15 9 '.'
    return , $g
}
function Save([object]$g, [string]$name) { Save-PixelArt -Rows (Rows $g) -Palette $pal -Path "$blocks/transmission_$name.png" }

Save (Andesite-Casing) 'andesite_casing'
Save (With-Hole (Andesite-Casing) 'd') 'andesite_end'
Save (Brass-Casing) 'brass_casing'
Save (With-Hole (Brass-Casing) 'z') 'brass_end'
Save (With-Slot (Andesite-Casing) 'd') 'andesite_cog_side'
Save (With-Slot (Brass-Casing) 'z') 'brass_cog_side'

# gearbox: andesite casing with a cross plate on its no-shaft faces
$g = Andesite-Casing; Set-Rect $g 4 7 11 8 'c'; Set-Rect $g 7 4 8 11 'c'; Set-Rect $g 6 6 9 9 'd'; Set-Rect $g 7 7 8 8 's'
Save $g 'gearbox_top'

# clutch / gearshift sides: a lamp strip (dark off, red when powered); gearshift has arrows
function Split-Side([bool]$on, [bool]$arrows) {
    $g = Andesite-Casing
    Set-Rect $g 3 6 12 9 'd'
    $lamp = if ($on) { 'r' } else { 'u' }
    Set-Rect $g 4 7 11 8 $lamp
    if ($on) { Set-Pixel $g 4 7 'p'; Set-Pixel $g 5 7 'p' }
    if ($arrows) {
        foreach ($x in 4, 11) { Set-Pixel $g $x 4 'o'; Set-Pixel $g $x 11 'o' }
        Set-Rect $g 5 3 10 3 'o'; Set-Rect $g 5 12 10 12 'o'
    }
    return , $g
}
Save (Split-Side $false $false) 'clutch_side'
Save (Split-Side $true $false) 'clutch_side_on'
Save (Split-Side $false $true) 'gearshift_side'
Save (Split-Side $true $true) 'gearshift_side_on'

# chain drive: a chain loop seen through the casing
function Chain-Side([int]$lamp) {
    $g = Andesite-Casing
    Set-Rect $g 3 3 12 12 'd'
    Set-Frame $g 4 4 11 11 't'
    for ($i = 4; $i -le 11; $i += 2) { Set-Pixel $g $i 4 's'; Set-Pixel $g $i 11 's'; Set-Pixel $g 4 $i 's'; Set-Pixel $g 11 $i 's' }
    Set-Rect $g 6 6 9 9 'u'
    if ($lamp -ge 0) {
        $c = if ($lamp -gt 0) { 'j' } else { 'i' }
        Set-Rect $g 7 7 8 8 $c
    }
    return , $g
}
Save (Chain-Side -1) 'chain_side'
Save (Chain-Side 0) 'chain_gearshift_side'
Save (Chain-Side 1) 'chain_gearshift_side_on'

# sequenced gearshift: brass casing with 5 lamps, the first `n` lit (the running instruction)
for ($n = 0; $n -le 5; $n++) {
    $g = Brass-Casing
    Set-Rect $g 3 6 12 9 'm'
    for ($i = 0; $i -lt 5; $i++) {
        $x = 3 + $i * 2
        $c = if ($i -lt $n) { 'n' } else { 'u' }
        Set-Rect $g $x 7 $x 8 $c
    }
    Save $g "sequencer_side_$n"
}

# rotation speed controller: brass with a value panel; top with the bracket rail
$g = Brass-Casing; Set-Rect $g 4 3 11 6 'm'; Set-Frame $g 4 3 11 6 'z'; Set-Rect $g 6 4 9 5 'j'
Save $g 'controller_side'
$g = Brass-Casing; Set-Rect $g 0 6 15 9 'z'; Set-Rect $g 0 7 15 8 'y'
Save $g 'controller_top'

# gauge faces: a dial plate with the scale (needle drawn by the model)
function Gauge-Face([char]$low, [char]$mid, [char]$high) {
    $g = Andesite-Casing
    Set-Rect $g 2 2 13 13 'b'
    Set-Frame $g 2 2 13 13 'd'
    # scale arc around the pivot (8, 9 from the top): left = 0, top = 1.0
    foreach ($p in @(@(3, 9, $low), @(3, 8, $low), @(4, 6, $low), @(5, 5, $mid), @(6, 4, $mid), @(8, 3, $mid), @(10, 4, $high), @(11, 5, $high), @(12, 6, $high))) {
        Set-Pixel $g $p[0] $p[1] $p[2]
    }
    Set-Rect $g 7 9 8 10 'o'
    return , $g
}
Save (Gauge-Face 'n' 'i' 'j') 'speedometer_head'
Save (Gauge-Face 'n' 'y' 'r') 'stressometer_head'

# ---- rotor atlas -------------------------------------------------------------------------------
$r = New-Grid 64 64 '.'
# shaft side and end
for ($y = 0; $y -le 15; $y++) { $r[$y][0] = 'u'; $r[$y][1] = 's'; $r[$y][2] = 't'; $r[$y][3] = 'u' }
for ($y = 0; $y -le 15; $y += 4) { $r[$y][1] = 't'; $r[$y][2] = 'u' }
Set-Rect $r 4 0 7 3 't'; Set-Rect $r 5 1 6 2 's'
# small cog hub top (12x12 disc) and side
Set-Rect $r 8 0 19 11 'f'
Set-Frame $r 8 0 19 11 'g'
Set-Frame $r 10 2 17 9 'e'
Set-Rect $r 12 4 15 7 't'; Set-Rect $r 13 5 14 6 's'
foreach ($p in @(@(8, 0), @(19, 0), @(8, 11), @(19, 11))) { Set-Pixel $r $p[0] $p[1] '.' }
Set-Rect $r 8 12 19 15 'f'; Set-Rect $r 8 12 19 12 'e'; Set-Rect $r 8 15 19 15 'h'
# small tooth top / side / end
Set-Rect $r 20 0 35 1 'e'; Set-Rect $r 26 0 29 1 'f'
Set-Rect $r 20 2 35 5 'f'; Set-Rect $r 20 2 35 2 'e'; Set-Rect $r 20 5 35 5 'h'; Set-Rect $r 26 2 29 5 'g'
Set-Rect $r 36 0 37 3 'g'
# large cog hub top (20x20) and side
Set-Rect $r 0 20 19 39 'f'
Set-Frame $r 0 20 19 39 'g'
Set-Frame $r 3 23 16 36 'e'
Set-Frame $r 6 26 13 33 'g'
Set-Rect $r 8 28 11 31 't'; Set-Rect $r 9 29 10 30 's'
foreach ($p in @(@(0, 20), @(1, 20), @(0, 21), @(19, 20), @(18, 20), @(19, 21), @(0, 39), @(1, 39), @(0, 38), @(19, 39), @(18, 39), @(19, 38))) { Set-Pixel $r $p[0] $p[1] '.' }
Set-Rect $r 0 40 19 43 'f'; Set-Rect $r 0 40 19 40 'e'; Set-Rect $r 0 43 19 43 'h'
# large tooth top / side / end
Set-Rect $r 20 20 43 22 'e'; Set-Rect $r 30 20 33 22 'f'
Set-Rect $r 20 23 43 26 'f'; Set-Rect $r 20 23 43 23 'e'; Set-Rect $r 20 26 43 26 'h'; Set-Rect $r 30 23 33 26 'g'
Set-Rect $r 44 20 46 23 'g'
# gauge needle (red tip)
Set-Rect $r 48 0 51 7 'o'; Set-Rect $r 48 0 51 2 'r'
Save-PixelArt -Rows (Rows $r) -Palette $pal -Path "$blocks/transmission_rotor.png"

# ---- vertical gearbox item -----------------------------------------------------------------------
$it = New-Grid 16 16 '.'
Set-Rect $it 3 3 12 12 'w'; Set-Frame $it 3 3 12 12 'v'; Set-Rect $it 5 5 10 10 'a'; Set-Frame $it 5 5 10 10 'x'
Set-Rect $it 7 0 8 15 't'; Set-Rect $it 7 0 7 15 's'
Set-Rect $it 0 7 15 8 't'; Set-Rect $it 0 7 15 7 's'
Set-Rect $it 6 6 9 9 'd'; Set-Rect $it 7 7 8 8 'u'
Save-PixelArt -Rows (Rows $it) -Palette $pal -Path "$items/vertical_gearbox.png"
