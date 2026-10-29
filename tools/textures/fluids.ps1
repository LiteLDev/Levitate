# Textures of the fluids unit (U19): spout, item drain, hose pulley, portable fluid interface,
# honey / chocolate source blocks and buckets. Original pixel art (copper casing style).
#
#   pwsh tools/textures/fluids.ps1            # writes assets/pack/rp/textures/{blocks,items}/
#   pwsh tools/textures/fluids.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
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

# palette keys are case-insensitive: one letter per colour
$pal = @{
    C = '#c46e43'; L = '#e3946a'; D = '#94502f'; K = '#5e2f1b'; A = '#2b160c'; H = '#f2b48a'
    G = '#6d6f68'; Q = '#4a4c46'; S = '#9fa29a'
    Y = '#eaae2f'; O = '#c98d1c'; W = '#ffd77a'
    B = '#6b2a1c'; N = '#4c1c12'; V = '#8d4430'
    I = '#c9cdd0'; R = '#8e9396'; J = '#5f6366'
}
# honey is a little translucent in the world (blend render method)
$honeyPal = @{ Y = '#e6eaae2f'; O = '#e6c98d1c'; W = '#e6ffd77a' }

# copper casing panel in a 16x16 cell at (ox, oy): bevelled frame, plate, corner rivets
function Casing($c, $ox, $oy) {
    for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        $k = if ((($x * 5 + $y * 3) % 13) -eq 0) { 'L' } elseif ((($x + $y * 7) % 17) -eq 0) { 'D' } else { 'C' }
        Set-Px $c ($ox + $x) ($oy + $y) $k
    } }
    Fill $c $ox $oy ($ox + 15) $oy 'H'; Fill $c $ox $oy $ox ($oy + 15) 'H'
    Fill $c $ox ($oy + 15) ($ox + 15) ($oy + 15) 'K'; Fill $c ($ox + 15) $oy ($ox + 15) ($oy + 15) 'K'
    Fill $c ($ox + 1) ($oy + 1) ($ox + 14) ($oy + 1) 'L'; Fill $c ($ox + 1) ($oy + 14) ($ox + 14) ($oy + 14) 'D'
    foreach ($p in @(@(2, 2), @(13, 2), @(2, 13), @(13, 13))) { Set-Px $c ($ox + $p[0]) ($oy + $p[1]) 'A'; Set-Px $c ($ox + $p[0] - 1) ($oy + $p[1] - 1) 'H' }
}
# a round opening of radius r centred in a 16x16 cell
function Hole($c, $ox, $oy, [double]$r, [char]$k = 'A', [char]$rim = 'K') {
    for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        $d = [math]::Sqrt([math]::Pow($x - 7.5, 2) + [math]::Pow($y - 7.5, 2))
        if ($d -le $r) { Set-Px $c ($ox + $x) ($oy + $y) $k } elseif ($d -le $r + 1) { Set-Px $c ($ox + $x) ($oy + $y) $rim }
    } }
}

# --- spout (32x32): body side, body top (inlet), nozzle, body bottom ------------------------------
$sp = New-Canvas 32 32
Casing $sp 0 0
for ($x = 1; $x -le 14; $x++) { Set-Px $sp $x 9 'K' }       # lower lip of the body
Casing $sp 16 0; Hole $sp 16 0 3.2                          # top with the inlet
Casing $sp 16 16; Hole $sp 16 16 2.2 'K' 'D'                # bottom
for ($y = 16; $y -lt 20; $y++) { for ($x = 0; $x -lt 8; $x++) { Set-Px $sp $x $y ($(if ($y -eq 16) { 'L' } elseif ($y -eq 19) { 'K' } elseif ($x % 3 -eq 0) { 'D' } else { 'C' })) } }
for ($y = 16; $y -lt 18; $y++) { for ($x = 8; $x -lt 12; $x++) { Set-Px $sp $x $y ($(if ($y -eq 17) { 'K' } else { 'D' })) } }
Fill $sp 8 20 11 23 'D'; Fill $sp 9 21 10 22 'A'           # nozzle tip end
Fill $sp 0 24 7 31 'C'; Fill $sp 0 24 7 24 'L'; Fill $sp 0 31 7 31 'K'; Fill $sp 2 26 5 29 'D'; Fill $sp 3 27 4 28 'A'
Save $sp $pal (Join-Path $blocks 'spout.png')

# --- item drain (32x32): side (13 px used), grate top, bottom -------------------------------------
$dr = New-Canvas 32 32
Casing $dr 0 0
Fill $dr 0 3 15 3 'H'; Fill $dr 2 7 13 8 'K'; Fill $dr 2 9 13 9 'D'   # a slot on the sides
Casing $dr 16 0
Fill $dr 18 2 29 13 'A'
for ($i = 18; $i -le 29; $i += 3) { Fill $dr $i 2 $i 13 'G'; Fill $dr ($i + 1) 2 ($i + 1) 13 'Q' }
for ($i = 2; $i -le 13; $i += 4) { Fill $dr 18 $i 29 $i 'S' }
Casing $dr 0 16
Save $dr $pal (Join-Path $blocks 'item_drain.png')

# --- hose pulley (48x32): shaft side, front coil, back pipe, bottom hose, top; shaft ---------------
$hp = New-Canvas 48 32
Casing $hp 0 0; Hole $hp 0 0 2.0 'G' 'Q'                   # sides: shaft opening
Casing $hp 16 0                                            # front: the hose coil
for ($y = 3; $y -le 12; $y++) { for ($x = 19; $x -le 28; $x++) { Set-Px $hp $x $y ($(if (($y % 2) -eq 0) { 'N' } else { 'B' })) } }
Fill $hp 19 3 28 3 'V'
Casing $hp 0 16; Hole $hp 0 16 3.5 'A' 'G'                 # back: pipe connection
Casing $hp 16 16; Hole $hp 16 16 2.8 'N' 'A'               # bottom: hose outlet
Casing $hp 32 16
for ($x = 32; $x -lt 48; $x++) { for ($y = 0; $y -lt 4; $y++) { Set-Px $hp $x $y ($(if ($y -eq 0) { 'S' } elseif ($y -eq 3) { 'Q' } elseif (($x % 4) -eq 0) { 'Q' } else { 'G' })) } }
Fill $hp 32 4 35 7 'G'; Set-Px $hp 33 5 'S'; Set-Px $hp 34 6 'Q'
Save $hp $pal (Join-Path $blocks 'hose_pulley.png')

# --- portable fluid interface (16x16 each) ----------------------------------------------------------
$pf = New-Canvas; Casing $pf 0 0
Fill $pf 3 3 12 12 'K'; Fill $pf 4 4 11 11 'A'
for ($y = 5; $y -le 10; $y += 2) { Fill $pf 4 $y 11 $y 'I' }
Fill $pf 4 4 11 4 'R'
Save $pf $pal (Join-Path $blocks 'portable_fluid_interface_front.png')
$pb = New-Canvas; Casing $pb 0 0; Hole $pb 0 0 3.0 'A' 'G'
Save $pb $pal (Join-Path $blocks 'portable_fluid_interface_back.png')
$ps = New-Canvas; Casing $ps 0 0; Fill $ps 2 6 13 9 'D'; Fill $ps 2 7 13 8 'K'
Save $ps $pal (Join-Path $blocks 'portable_fluid_interface_side.png')

# --- honey / chocolate source blocks (still fluid look) --------------------------------------------
function Fluid([char]$base, [char]$dark, [char]$light) {
    $c = New-Canvas
    for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        $v = [math]::Sin(($x + $y * 0.6) * 0.9) + [math]::Cos(($y - $x * 0.4) * 0.7)
        $k = if ($v -gt 1.2) { $light } elseif ($v -lt -1.1) { $dark } else { $base }
        Set-Px $c $x $y $k
    } }
    , $c
}
Save (Fluid 'Y' 'O' 'W') $honeyPal (Join-Path $blocks 'honey_still.png')
Save (Fluid 'B' 'N' 'V') $pal (Join-Path $blocks 'chocolate_still.png')

# --- buckets (iron bucket with the fluid at the top) ----------------------------------------------
function Bucket([char]$f, [char]$top) {
    $c = New-Canvas
    $shape = @(
        '................',
        '................',
        '....RRRRRRRR....',
        '...RIIIIIIIIR...',
        '..RJ########JR..',
        '..RJ########JR..',
        '..RIJJJJJJJJIR..',
        '...RIIIIIIIIR...',
        '...RIIIIIIIIR...',
        '....RIIIIIIR....',
        '....RIIIIIIR....',
        '....RIIIIIIR....',
        '.....RIIIIR.....',
        '.....RRRRRR.....',
        '................',
        '................'
    )
    for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        $ch = $shape[$y][$x]
        if ($ch -eq '#') { $ch = if ($y -eq 4) { $top } else { $f } }
        if ($ch -ne '.') { Set-Px $c $x $y $ch }
    } }
    , $c
}
Save (Bucket 'Y' 'W') $pal (Join-Path $items 'honey_bucket.png')
Save (Bucket 'B' 'V') $pal (Join-Path $items 'chocolate_bucket.png')
