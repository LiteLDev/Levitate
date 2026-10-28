# Textures of the tracks unit (U5): original pixel art.
#
#   pwsh tools/textures/tracks.ps1           # writes assets/pack/rp/textures/{blocks,items}/...
#   pwsh tools/textures/tracks.ps1 -Preview  # also writes 8x previews to tools/textures/preview/ (ignored)
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$blocks = Join-Path $root 'assets/pack/rp/textures/blocks'
$items = Join-Path $root 'assets/pack/rp/textures/items'

function Save([string[]]$rows, [hashtable]$pal, [string]$path) {
    Save-PixelArt -Rows $rows -Palette $pal -Path $path
    if ($Preview) { Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $PSScriptRoot ('preview/' + [IO.Path]::GetFileName([IO.Path]::GetDirectoryName($path)) + '-' + [IO.Path]::GetFileName($path))) -Scale 8 }
}

# ---- palette ------------------------------------------------------------------------------------
$wood = '#5b4330'          # dark spruce sleepers
$woodHi = Shade-Color $wood 1.25
$woodLo = Shade-Color $wood 0.7
$metal = '#a7a89f'         # andesite alloy rails
$metalHi = Shade-Color $metal 1.3
$metalLo = Shade-Color $metal 0.68
$gravel = '#77746c'
$gravelLo = Shade-Color $gravel 0.75

# ---- track block (32x32): sleepers top-left 16x16, rail top-right 16x16 ----------------------------
$tie = @(
    'hhhhhhhhhhhhhhhh',
    'wwwwhwwwwwwhwwww',
    'wwwwwwwwlwwwwwww',
    'wlwwwwwwwwwwwlww',
    'wwwwwwhwwwwwwwww',
    'wwwwwwwwwwwwwwww',
    'wwhwwwwwwwlwwwww',
    'wwwwwwwwwwwwwwhw',
    'wwwwwlwwwwwwwwww',
    'wwwwwwwwwhwwwwww',
    'whwwwwwwwwwwwwww',
    'wwwwwwwwwwwwlwww',
    'wwwwlwwwwwwwwwww',
    'wwwwwwwwwwwhwwww',
    'wwwwwwwwwwwwwwww',
    'llllllllllllllll'
)
$rail = @(
    'HHHHHHHHHHHHHHHH',
    'HmmmmmmmmmmmmmmH',
    'mmmmmmmmmmmmmmmm',
    'mmmmmmmmmmmmmmmm',
    'mmmmmmmmmmmmmmmm',
    'mmmmmmmmmmmmmmmm',
    'mmmmmmmmmmmmmmmm',
    'mmmmmmmmmmmmmmmm',
    'mmmmmmmmmmmmmmmm',
    'mmmmmmmmmmmmmmmm',
    'mmmmmmmmmmmmmmmm',
    'mmmmmmmmmmmmmmmm',
    'mmmmmmmmmmmmmmmm',
    'mmmmmmmmmmmmmmmm',
    'LmmmmmmmmmmmmmmL',
    'LLLLLLLLLLLLLLLL'
)
$track = @()
for ($y = 0; $y -lt 16; $y++) { $track += $tie[$y] + $rail[$y] }
for ($y = 0; $y -lt 16; $y++) { $track += ('.' * 32) }
# PowerShell hashtables are case-insensitive: the rail highlights use their own keys (r, d)
$trackPal = @{ w = $wood; h = $woodHi; l = $woodLo; m = $metal; r = $metalHi; d = $metalLo }
$track = $track | ForEach-Object { $_.Replace('H', 'r').Replace('L', 'd') }
Save $track $trackPal (Join-Path $blocks 'track.png')

# ---- fake track (16x16): the track bed vanilla clients see along a curve -------------------------
$fake = @(
    '.g.....k....g...',
    '...wwww...wwww..',
    'k..wwww.g.wwww.g',
    '.......k........',
    'g.k.........k...',
    '..wwww..g.wwww..',
    '..wwww....wwww.k',
    '.g.....k.......g',
    '....g........k..',
    '.wwww..k..wwww..',
    '.wwww.....wwww..',
    'k......g.......k',
    '..g.......k.....',
    '...wwww..g.wwww.',
    'g..wwww....wwww.',
    '.......k.g......'
)
Save $fake @{ w = $woodLo; g = $gravel; k = $gravelLo } (Join-Path $blocks 'fake_track.png')

# ---- item icons (16x16) -----------------------------------------------------------------------------
$icon = @(
    '................',
    '...r.......r....',
    '.lwmwwwwwwwmwl..',
    '.lwdwwwwwwwdwl..',
    '...m.......m....',
    '...m.......m....',
    '.lwmwwwwwwwmwl..',
    '.lwdwwwwwwwdwl..',
    '...m.......m....',
    '...m.......m....',
    '.lwmwwwwwwwmwl..',
    '.lwdwwwwwwwdwl..',
    '...m.......m....',
    '...d.......d....',
    '................',
    '................'
)
Save $icon $trackPal (Join-Path $items 'track.png')

$incomplete = @(
    '................',
    '................',
    '................',
    '...r.......r....',
    '.sssmsssssssss..',
    '.sttdtttttttts..',
    '...m............',
    '...m............',
    '.sssmsssssssss..',
    '.sttdtttttttts..',
    '...m............',
    '...d............',
    '.ssssssssssss...',
    '.stttttttttts...',
    '................',
    '................'
)
Save $incomplete @{ s = '#9c9c97'; t = '#7a7a75'; m = $metal; r = $metalHi; d = $metalLo } (Join-Path $items 'incomplete_track.png')

Write-Host 'tracks textures written'
