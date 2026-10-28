# Textures of the kinetics unit's test blocks (original pixel art).
#   pwsh tools/textures/kinetics.ps1
#
# Layout shared by the test block models (models/blocks/kinetics_test_*.geo.json, 16x16):
#   x 0..3,  y 0..15  shaft side          x 4..7, y 0..3   shaft end
#   x 4..7,  y 4..7   cog tooth top       x 4..7, y 8..11  cog tooth side
#   x 8..15, y 0..7   cog hub top / casing face
#   x 8..15, y 8..11  cog hub side
. "$PSScriptRoot/lib.ps1"
$out = Join-Path $PSScriptRoot '../../assets/pack/rp/textures/blocks'

$shaftSide = @('bcab', 'bcab', 'bcab', 'beeb')
$shaftEnd  = @('beeb', 'eooe', 'eooe', 'beeb')

function Compose([string[]]$left4x16, [string[]]$endCap, [string[]]$tooth8, [string[]]$hub12) {
    # left4x16: 16 rows of 4; endCap: 4 rows of 4; tooth8: 8 rows of 4 (rows 4..11);
    # hub12: 12 rows of 8 (rows 0..11 of x 8..15)
    $rows = @()
    for ($y = 0; $y -lt 16; $y++) {
        $a = $left4x16[$y]
        if ($y -lt 4) { $b = $endCap[$y] }
        elseif ($y -lt 12 -and $tooth8) { $b = $tooth8[$y - 4] }
        else { $b = '....' }
        if ($y -lt 12 -and $hub12) { $c = $hub12[$y] } else { $c = '........' }
        $rows += "$a$b$c"
    }
    return , $rows
}

$side16 = @(); for ($i = 0; $i -lt 4; $i++) { $side16 += $shaftSide }

$basePalette = @{
    b = '#6b706d'; c = '#a9aeab'; a = '#8a8f8c'; e = '#4a4e4c'; o = '#2f2f2f'
}

# shaft
Save-PixelArt -Rows (Compose $side16 $shaftEnd $null $null) -Palette $basePalette -Path "$out/kinetics_test_shaft.png"

# cogs (small and large share it)
# w light wood, k mid, n dark, d darkest (hashtable keys are case-insensitive: distinct letters)
$tooth = @('nndd', 'nwwn', 'nwwn', 'dnnd', 'dnnd', 'nddn', 'nddn', 'dnnd')
$hub = @(
    'kkkkkkkk',
    'kwwwwwwk',
    'kwkkkkwk',
    'kwkookwk',
    'kwkookwk',
    'kwkkkkwk',
    'kwwwwwwk',
    'kkkkkkkk',
    'dnnnnnnd',
    'nwwwwwwn',
    'nwwwwwwn',
    'dnnnnnnd'
)
$cogPalette = $basePalette.Clone()
$cogPalette.w = '#a0794a'; $cogPalette.k = '#7d5a33'; $cogPalette.n = '#6b4a2a'; $cogPalette.d = '#55391f'
Save-PixelArt -Rows (Compose $side16 $shaftEnd $tooth $hub) -Palette $cogPalette -Path "$out/kinetics_test_cog.png"

# source (red casing) and consumer (blue casing)
function Casing([string]$light, [string]$dark) {
    return @(
        'ffffffff',
        "f${light}${light}${light}${light}${light}${light}f",
        "f${light}${dark}${dark}${dark}${dark}${light}f",
        "f${light}${dark}oo${dark}${light}f",
        "f${light}${dark}oo${dark}${light}f",
        "f${light}${dark}${dark}${dark}${dark}${light}f",
        "f${light}${light}${light}${light}${light}${light}f",
        'ffffffff',
        '........', '........', '........', '........'
    )
}
$srcPalette = $basePalette.Clone(); $srcPalette.f = '#3d3d3d'; $srcPalette.r = '#b5432f'; $srcPalette.s = '#7e2b1f'
Save-PixelArt -Rows (Compose $side16 $shaftEnd $null (Casing 'r' 's')) -Palette $srcPalette -Path "$out/kinetics_test_source.png"
$conPalette = $basePalette.Clone(); $conPalette.f = '#3d3d3d'; $conPalette.u = '#3f6fb5'; $conPalette.v = '#284a7e'
Save-PixelArt -Rows (Compose $side16 $shaftEnd $null (Casing 'u' 'v')) -Palette $conPalette -Path "$out/kinetics_test_consumer.png"
