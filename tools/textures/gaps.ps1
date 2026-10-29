# Textures of the U37 `gaps` unit: the rotation indicator particle (Create's goggles particle
# that circles a generator when its speed level changes). Original pixel art: a white glint that
# shrinks over 8 frames (the particle tints it with the speed level colour).
#
#   pwsh tools/textures/gaps.ps1            # writes assets/pack/rp/textures/particle/levitate_rotation_indicator.png
#   pwsh tools/textures/gaps.ps1 -Preview   # also writes an 8x preview to tools/textures/preview/ (not committed)
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$dir = Join-Path $PSScriptRoot '../../assets/pack/rp/textures/particle'
New-Item -ItemType Directory -Force $dir | Out-Null

# eight 8x8 frames side by side (64x8): a four-pointed glint, then a dot fading out
$frames = @(
    @('...a....', '...m....', '..ama...', 'ammomma.', '..ama...', '...m....', '...a....', '........'),
    @('........', '...a....', '..ama...', '.amoma..', '..ama...', '...a....', '........', '........'),
    @('........', '...a....', '..aoa...', '.aoooa..', '..aoa...', '...a....', '........', '........'),
    @('........', '........', '..ama...', '..mom...', '..ama...', '........', '........', '........'),
    @('........', '........', '...a....', '..aoa...', '...a....', '........', '........', '........'),
    @('........', '........', '...b....', '..bmb...', '...b....', '........', '........', '........'),
    @('........', '........', '........', '...m....', '........', '........', '........', '........'),
    @('........', '........', '........', '...b....', '........', '........', '........', '........')
)
$rows = @()
for ($y = 0; $y -lt 8; $y++) {
    $line = ''
    foreach ($f in $frames) { $line += $f[$y] }
    $rows += $line
}
$palette = @{ 'o' = '#FFFFFFFF'; 'm' = '#E6FFFFFF'; 'a' = '#70FFFFFF'; 'b' = '#40FFFFFF' }
$path = Join-Path $dir 'levitate_rotation_indicator.png'
Save-PixelArt -Rows $rows -Palette $palette -Path $path
if ($Preview) {
    $p = Join-Path $PSScriptRoot 'preview'
    New-Item -ItemType Directory -Force $p | Out-Null
    Save-PixelArt -Rows $rows -Palette $palette -Path (Join-Path $p 'levitate_rotation_indicator.png') -Scale 8
}
Write-Host "gaps textures written to $dir"
