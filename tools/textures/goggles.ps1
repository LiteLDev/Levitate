# Textures of the U8 `goggles` unit: the wrench and the engineer's goggles items and the goggles'
# worn model texture. Original pixel art (not derived from Create's assets).
#
#   pwsh tools/textures/goggles.ps1            # writes assets/pack/rp/textures/{items,models}/*.png
#   pwsh tools/textures/goggles.ps1 -Preview   # also writes 8x previews to tools/textures/preview/ (not committed)
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$items  = Join-Path $PSScriptRoot '../../assets/pack/rp/textures/items'
$models = Join-Path $PSScriptRoot '../../assets/pack/rp/textures/models'
New-Item -ItemType Directory -Force $items, $models | Out-Null

function Save-All([string[]]$Rows, [hashtable]$Palette, [string]$Path) {
    Save-PixelArt -Rows $Rows -Palette $Palette -Path $Path
    if ($Preview) {
        $dir = Join-Path $PSScriptRoot 'preview'
        New-Item -ItemType Directory -Force $dir | Out-Null
        Save-PixelArt -Rows $Rows -Palette $Palette -Path (Join-Path $dir ([IO.Path]::GetFileName($Path))) -Scale 8
    }
}

# The wrench: a golden open-jaw head with a small cog on the shaft, a wooden grip.
$wrench = @(
    '................',
    '..........dd....',
    '.........daad...',
    '........day.....',
    '........day..dd.',
    '........daaddaad',
    '.........daaaayd',
    '........cdaayyd.',
    '.......ckcdddd..',
    '......ckokc.....',
    '.....wckcc......',
    '....whwc........',
    '...whw..........',
    '..whw...........',
    '.whw............',
    '.ww.............'
)
$wrenchPal = @{
    d = '#7a4f12'; a = '#f2c94c'; y = '#c99a2e'
    c = '#5a5a60'; k = '#b4b4bc'; o = '#2e2e33'
    w = '#4a2f17'; h = '#9a6a3a'
}
Save-All $wrench $wrenchPal (Join-Path $items 'wrench.png')

# The goggles: two round brass-rimmed lenses on a leather strap.
$goggles = @(
    '................',
    '................',
    '................',
    '................',
    '..bbb......bbb..',
    '.brrrb....brrrb.',
    'srlelrbssbrlelrs',
    'srelerbbbbrelers',
    'srlelrb..brlelrs',
    '.brrrb....brrrb.',
    '..bbb......bbb..',
    '................',
    '................',
    '................',
    '................',
    '................'
)
$gogglesPal = @{
    b = '#6b4a14'; r = '#e0b03c'; s = '#5c3a1e'
    l = '#7fc6d8'; e = '#cdeef5'
}
Save-All $goggles $gogglesPal (Join-Path $items 'goggles.png')

# Worn model texture (64x32, box UV of models/entity/goggles.geo.json):
#   strap  9x2x9 at (0,0): sides only (top / bottom transparent)
#   lenses 3x3x1 at (0,12) and (10,12), held copy at (20,12) and (30,12)
$w = 64; $h = 32
$grid = for ($y = 0; $y -lt $h; $y++) { , ([char[]]('.' * $w)) }
function Fill([int]$x, [int]$y, [int]$fw, [int]$fh, [char]$c) {
    for ($j = $y; $j -lt $y + $fh; $j++) { for ($i = $x; $i -lt $x + $fw; $i++) { $grid[$j][$i] = $c } }
}
# strap: side faces live in rows 9..10 (v + d .. v + d + h), x 0..35
Fill 0 9 36 2 's'
for ($i = 0; $i -lt 36; $i += 3) { $grid[9][$i] = 't' }
foreach ($u in 0, 10, 20, 30) {
    # a 3x3x1 cube: top (u+1,12) 3x1, bottom (u+4,12) 3x1, sides at row 13..15
    Fill ($u + 1) 12 3 1 'r'
    Fill ($u + 4) 12 3 1 'b'
    Fill $u 13 1 3 'b'
    Fill ($u + 4) 13 1 3 'b'
    Fill ($u + 5) 13 3 3 'r'
    # front face (u+1,13) 3x3: rim with a glass centre
    Fill ($u + 1) 13 3 3 'r'
    $grid[14][$u + 2] = 'e'
}
$rows = foreach ($r in $grid) { -join $r }
$modelPal = @{ s = '#5c3a1e'; t = '#7a5230'; b = '#6b4a14'; r = '#e0b03c'; e = '#9fdcea' }
Save-All $rows $modelPal (Join-Path $models 'goggles.png')
