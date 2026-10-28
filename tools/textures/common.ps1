# Textures of the U3 `common` unit: the list filter and the attribute filter items.
# Original pixel art (not derived from Create's assets).
#
#   pwsh tools/textures/common.ps1            # writes assets/pack/rp/textures/items/*.png
#   pwsh tools/textures/common.ps1 -Preview   # also writes 8x previews to tools/textures/preview/ (not committed)
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$out = Join-Path $PSScriptRoot '../../assets/pack/rp/textures/items'
New-Item -ItemType Directory -Force $out | Out-Null

# A round frame holding a mesh: iron for the list filter, brass for the attribute filter.
$frame = @(
    '................',
    '.....oooooo.....',
    '...ooAAAAAAoo...',
    '..oAAbbbbbbCCo..',
    '..oAb######dCo..',
    '.oAb#~#~#~#~dCo.',
    '.oAb~#~#~#~#dCo.',
    '.oAb#~#~#~#~dCo.',
    '.oAb~#~#~#~#dCo.',
    '.oAb#~#~#~#~dCo.',
    '.oAb~#~#~#~#dCo.',
    '..oAb######dCo..',
    '..oCCddddddCCo..',
    '...ooCCCCCCoo...',
    '.....oooooo.....',
    '................'
)

$iron = @{
    o = '#2b2b30'; A = '#e8e8ee'; b = '#c3c3cc'; C = '#8a8a96'; d = '#a6a6b0'
    '#' = '#f3efe4'; '~' = '#cfc8b6'
}
Save-PixelArt -Rows $frame -Palette $iron -Path (Join-Path $out 'filter.png')

# The attribute filter: brass frame, paper-coloured weave with a tag mark.
$tag = @(
    '................',
    '................',
    '................',
    '................',
    '................',
    '......tt........',
    '.....trrt.......',
    '.....trrrt......',
    '......trrrt.....',
    '.......trrt.....',
    '........tt......',
    '................',
    '................',
    '................',
    '................',
    '................'
)
$brass = @{
    o = '#3a2a12'; A = '#fff1a8'; b = '#f2cf5c'; C = '#a8741f'; d = '#d29d36'
    '#' = '#efe6d0'; '~' = '#d9ccad'; t = '#5a3d1a'; r = '#e05a3a'
}
Save-PixelArt -Rows (Merge-Rows $frame $tag) -Palette $brass -Path (Join-Path $out 'attribute_filter.png')

if ($Preview) {
    $pv = Join-Path $PSScriptRoot 'preview'
    New-Item -ItemType Directory -Force $pv | Out-Null
    Save-PixelArt -Rows $frame -Palette $iron -Path (Join-Path $pv 'filter.png') -Scale 8
    Save-PixelArt -Rows (Merge-Rows $frame $tag) -Palette $brass -Path (Join-Path $pv 'attribute_filter.png') -Scale 8
}
Write-Host "common textures written to $out"
