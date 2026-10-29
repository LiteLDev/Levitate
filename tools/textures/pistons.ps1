# Textures of the pistons unit (U20): mechanical pistons, poles, rope / elevator pulleys, ropes,
# magnets, elevator contacts and gantries. Original pixel art.
#
#   pwsh tools/textures/pistons.ps1            # writes assets/pack/rp/textures/blocks/
#   pwsh tools/textures/pistons.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$blocks = Join-Path $root 'assets/pack/rp/textures/blocks'

function New-Canvas([char]$fill = '.') { , (0..15 | ForEach-Object { , ([char[]]((New-Object string $fill, 16))) }) }
function Set-Px($c, $x, $y, [char]$k) { if ($x -ge 0 -and $x -lt 16 -and $y -ge 0 -and $y -lt 16) { $c[$y][$x] = $k } }
function Fill($c, $x0, $y0, $x1, $y1, [char]$k) { for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y $k } } }
function Rows($c) { , ($c | ForEach-Object { -join $_ }) }
function Save($c, $pal, $name) {
    $rows = Rows $c
    Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $blocks "$name.png")
    if ($Preview) { Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $root "tools/textures/preview/$name.png") -Scale 8 }
}

# andesite frame (light top-left, dark bottom-right) around a filling
function Frame($c, [int]$w = 2) {
    Fill $c 0 0 15 ($w - 1) 'F'; Fill $c 0 (16 - $w) 15 15 'F'; Fill $c 0 0 ($w - 1) 15 'F'; Fill $c (16 - $w) 0 15 15 'F'
    Fill $c 0 0 15 0 'H'; Fill $c 0 0 0 15 'H'; Fill $c 0 15 15 15 'S'; Fill $c 15 0 15 15 'S'
    foreach ($p in @(@(1, 1), @(14, 1), @(1, 14), @(14, 14))) { Set-Px $c $p[0] $p[1] 'n' }
}
function Planks($c, [int]$x0 = 0, [int]$y0 = 0, [int]$x1 = 15, [int]$y1 = 15) {
    for ($y = $y0; $y -le $y1; $y++) {
        $band = [math]::Floor($y / 4)
        for ($x = $x0; $x -le $x1; $x++) {
            $k = if ((($x + $band * 7) % 16) -eq 0) { 'g' } elseif ($y % 4 -eq 3) { 'd' } elseif (($x * 5 + $y * 3) % 13 -eq 0) { 'l' } else { 'p' }
            Set-Px $c $x $y $k
        }
    }
}
$andesite = @{ F = '#8e908a'; H = '#b4b6ae'; S = '#5f615b'; n = '#45463f'; p = '#a47b4a'; l = '#b88f5a'; d = '#7c5a33'; g = '#6b4c2a'
    '0' = '#2f2a24'; '1' = '#4a4239'; y = '#7fcb5e'; z = '#5c9f45'; w = '#c9f2b0'; r = '#8a6a45'; '2' = '#6e5232'; q = '#a8865a'
    m = '#9a9da3'; '3' = '#c4c7cc'; '4' = '#6b6e74'; t = '#b03a2e'; '5' = '#d65a48'; b = '#c9a447'; '6' = '#e8cf7a'; o = '#8f7128'
    e = '#4a3c30'; '7' = '#5d4b3c'; u = '#7d7f79'; x = '#9fa19b'; c = '#3c3a36'; '8' = '#f2d38a'; '9' = '#6d5a30'; v = '#ffeb9a'; '+' = '#b8933a' }

# ---- mechanical piston ---------------------------------------------------------------------------
$c = New-Canvas; Planks $c; Frame $c
Fill $c 2 10 13 11 'F'; Fill $c 2 10 13 10 'H'          # casing band under the head
Save $c $andesite 'piston_side'
$c = New-Canvas; Planks $c; Frame $c
foreach ($y in 4..11) { foreach ($x in 4..11) { $dx = $x - 7.5; $dy = $y - 7.5; $r = [math]::Sqrt($dx * $dx + $dy * $dy); if ($r -ge 2.4 -and $r -le 3.8) { Set-Px $c $x $y 'F' } } }
Save $c $andesite 'piston_back'
$c = New-Canvas '0'; Frame $c 3
foreach ($i in 3..12) { Set-Px $c $i 3 '1'; Set-Px $c 3 $i '1' }
Fill $c 6 6 9 9 'r'; Fill $c 6 6 9 6 'q'
Save $c $andesite 'piston_inner'
$c = New-Canvas; Planks $c; Fill $c 0 0 15 0 'H'; Fill $c 0 15 15 15 'S'
Save $c $andesite 'piston_head_side'
$c = New-Canvas; Planks $c; Frame $c
Fill $c 5 5 10 10 'F'; Fill $c 6 6 9 9 'S'; Fill $c 5 5 10 5 'H'
Save $c $andesite 'piston_front'
$c = New-Canvas; Planks $c; Frame $c
for ($y = 2; $y -le 13; $y++) { for ($x = 2; $x -le 13; $x++) { if ((($x + $y) % 3) -ne 0) { Set-Px $c $x $y ($(if (($x * 3 + $y) % 5 -eq 0) { 'z' } else { 'y' })) } } }
foreach ($p in @(@(4, 4), @(9, 6), @(6, 10), @(11, 11))) { Set-Px $c $p[0] $p[1] 'w' }
Save $c $andesite 'piston_front_sticky'

# ---- extension pole: wooden rod with andesite bands -------------------------------------------------
$c = New-Canvas
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        $k = if ($x % 4 -eq 0) { '2' } elseif ($x % 4 -eq 3) { 'r' } elseif (($y + $x) % 7 -eq 0) { 'q' } else { 'p' }
        Set-Px $c $x $y $k } }
foreach ($y in @(0, 1, 7, 8, 14, 15)) { Fill $c 0 $y 15 $y $(if ($y % 7 -eq 0 -or $y -eq 14) { 'H' } else { 'F' }) }
Save $c $andesite 'piston_pole'
$c = New-Canvas 'F'; Fill $c 0 0 15 0 'H'; Fill $c 0 0 0 15 'H'; Fill $c 5 5 10 10 'p'; Fill $c 6 6 9 9 'r'
Save $c $andesite 'piston_pole_end'

# ---- shaft (rotating part of pistons / pulleys) -----------------------------------------------------
$c = New-Canvas
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        $k = if ((($y + $x) % 4) -eq 0) { 'S' } elseif ($x % 4 -eq 1) { 'H' } else { 'x' }
        Set-Px $c $x $y $k } }
Save $c $andesite 'pistons_shaft'

# ---- rope pulley ------------------------------------------------------------------------------------
$c = New-Canvas; Planks $c; Frame $c
Fill $c 2 2 13 3 'F'; Fill $c 2 12 13 13 'F'
Save $c $andesite 'rope_pulley_side'
$c = New-Canvas; Planks $c; Frame $c
foreach ($y in 4..11) { foreach ($x in 4..11) { $dx = $x - 7.5; $dy = $y - 7.5; $r = [math]::Sqrt($dx * $dx + $dy * $dy); if ($r -le 3.0) { Set-Px $c $x $y 'r' } } }
Save $c $andesite 'rope_pulley_top'
$c = New-Canvas
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        $k = if ((($x + 2 * $y) % 6) -lt 2) { '2' } elseif ((($x + 2 * $y) % 6) -eq 5) { 'q' } else { 'r' }
        Set-Px $c $x $y $k } }
Save $c $andesite 'rope_coil'
$c = New-Canvas
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        $k = if ((($x + $y) % 4) -eq 0) { '2' } elseif ((($x + $y) % 4) -eq 2) { 'q' } else { 'r' }
        Set-Px $c $x $y $k } }
Save $c $andesite 'rope'
$c = New-Canvas 'm'
Fill $c 0 0 15 0 '3'; Fill $c 0 0 0 15 '3'; Fill $c 0 15 15 15 '4'; Fill $c 15 0 15 15 '4'
Fill $c 3 3 6 12 't'; Fill $c 9 3 12 12 't'; Fill $c 3 3 6 3 '5'; Fill $c 9 3 12 3 '5'; Fill $c 3 10 12 12 '4'
Save $c $andesite 'pulley_magnet'

# ---- elevator pulley (brass) ---------------------------------------------------------------------
$brass = @{} + $andesite; $brass['F'] = '#c9a447'; $brass['H'] = '#e8cf7a'; $brass['S'] = '#8f7128'; $brass['n'] = '#5e4a1a'
$c = New-Canvas; Planks $c; Frame $c; Fill $c 2 2 13 3 'F'; Fill $c 2 12 13 13 'F'
Save $c $brass 'elevator_pulley_side'
$c = New-Canvas; Planks $c; Frame $c; Fill $c 4 3 11 12 'e'; Fill $c 5 3 10 12 '7'
Save $c $brass 'elevator_pulley_top'
$c = New-Canvas 'e'
for ($y = 0; $y -lt 16; $y += 3) { Fill $c 0 $y 15 $y '7' }
Fill $c 0 0 0 15 'c'; Fill $c 15 0 15 15 'c'
Save $c $brass 'elevator_belt'

# ---- elevator contact (brass casing, red / lit lamp) ----------------------------------------------
$contact = @{} + $brass
$c = New-Canvas 'b'; Fill $c 0 0 15 0 '6'; Fill $c 0 0 0 15 '6'; Fill $c 0 15 15 15 'o'; Fill $c 15 0 15 15 'o'
Fill $c 3 3 12 12 '9'; Fill $c 4 4 11 11 '+'
Save $c $contact 'elevator_contact_side'
function Contact-Front($lamp, $lampHi) {
    $c = New-Canvas 'b'; Fill $c 0 0 15 0 '6'; Fill $c 0 0 0 15 '6'; Fill $c 0 15 15 15 'o'; Fill $c 15 0 15 15 'o'
    Fill $c 3 3 12 12 'c'
    Fill $c 5 5 10 10 $lamp; Fill $c 5 5 10 5 $lampHi; Fill $c 5 5 5 10 $lampHi
    , $c
}
Save (Contact-Front 't' '5') $contact 'elevator_contact_front'
Save (Contact-Front '9' '+') $contact 'elevator_contact_front_dim'
Save (Contact-Front '8' 'v') $contact 'elevator_contact_front_powered'

# ---- gantries --------------------------------------------------------------------------------------
$c = New-Canvas
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        $k = if ($y % 4 -eq 0) { 'S' } elseif ($y % 4 -eq 1) { 'H' } elseif ($x % 8 -lt 4) { 'x' } else { 'u' }
        Set-Px $c $x $y $k } }
Save $c $andesite 'gantry_shaft'
$c = New-Canvas
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        $k = if ($y % 4 -eq 0) { 'S' } elseif ($y % 4 -eq 1) { '5' } elseif ($x % 8 -lt 4) { 't' } else { 'u' }
        Set-Px $c $x $y $k } }
Save $c $andesite 'gantry_shaft_powered'
$c = New-Canvas 'F'; Fill $c 0 0 15 0 'H'; Fill $c 0 0 0 15 'H'; Fill $c 0 15 15 15 'S'; Fill $c 15 0 15 15 'S'; Fill $c 6 6 9 9 'n'
Save $c $andesite 'gantry_shaft_end'
$c = New-Canvas; Planks $c; Frame $c
Save $c $andesite 'gantry_carriage_side'
$c = New-Canvas; Planks $c; Frame $c; Fill $c 4 4 11 11 'F'; Fill $c 5 5 10 10 'p'; Fill $c 4 4 11 4 'H'
Save $c $andesite 'gantry_carriage_top'
$c = New-Canvas 'p'
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { if ((($x + $y) % 4) -eq 0) { Set-Px $c $x $y 'g' } elseif ($x % 4 -eq 1) { Set-Px $c $x $y 'l' } } }
Fill $c 0 0 15 0 'F'; Fill $c 0 15 15 15 'S'
Save $c $andesite 'gantry_pinion'
'pistons textures written'
