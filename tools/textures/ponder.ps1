# Textures of the Ponder unit (U30 / U46): the particle sprites Ponder scenes draw (flames, smoke,
# splashes, ...). Original pixel art; tinted by the renderer where a type needs it.
#
#   pwsh tools/textures/ponder.ps1   # writes assets/pack/rp/textures/particle/levitate_ponder_particles.png
#
# Layout: 8x8 cells, 8 per row (64 x 16):
#   row 0: flame, soul flame, smoke, cloud, splash, bubble, snowflake, heart
#   row 1: crit
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$out = Join-Path $root 'assets/pack/rp/textures/particle/levitate_ponder_particles.png'

$cells = @(
    # flame
    @('...yy...', '..yyy...', '..yoyy..', '.yoooy..', '.yorroy.', '.oorrro.', '..orro..', '...oo...'),
    # soul flame
    @('...cc...', '..ccc...', '..cbcc..', '.cbbbc..', '.cbnnbc.', '.bbnnnb.', '..bnnb..', '...bb...'),
    # smoke (grey, the renderer darkens it)
    @('..wwww..', '.wwwwww.', 'wwwlwwww', 'wwllwwww', 'wwwwwlww', 'wwwwwwww', '.wwwwww.', '..wwww..'),
    # cloud
    @('...ww...', '.wwwwww.', 'wwwwwwww', 'wwwwwwww', 'wlwwwwlw', 'wwwwwwww', '.wwwwww.', '...ww...'),
    # splash
    @('........', '...d....', '..dDd...', '..dDd..d', '...d..dD', '.d....d.', 'dDd.....', '.d......'),
    # bubble
    @('........', '..dddd..', '.d....d.', '.d.w..d.', '.d....d.', '.d....d.', '..dddd..', '........'),
    # snowflake
    @('...w....', '.w.w.w..', '..www...', 'wwwwwww.', '..www...', '.w.w.w..', '...w....', '........'),
    # heart
    @('........', '.rr.rr..', 'rRRrRRr.', 'rRRRRRr.', '.rRRRr..', '..rRr...', '...r....', '........')
)
$row2 = @(
    # crit
    @('...y....', '...y....', '...w....', 'yyw.wyy.', '...w....', '...y....', '...y....', '........')
)
# PowerShell hashtables are case-insensitive: map the upper-case keys to their own characters
$pal = @{ y = '#ffe14a'; o = '#ff9a1f'; r = '#c8321c'; c = '#a6f4ff'; b = '#3fd2e8'; n = '#1f8ea8'; w = '#f2f2f2'; l = '#c8c8c8'; d = '#3f76e4'; e = '#7fb2ff'; h = '#ff5a5a' }
function Fix([string[]]$rows) { , ($rows | ForEach-Object { $_.Replace('D', 'e').Replace('R', 'h') }) }

$rows = @()
for ($y = 0; $y -lt 8; $y++) {
    $line = ''
    foreach ($cell in $cells) { $line += (Fix $cell)[$y] }
    $rows += $line
}
for ($y = 0; $y -lt 8; $y++) {
    $line = ''
    foreach ($cell in $row2) { $line += (Fix $cell)[$y] }
    $line += '.' * (64 - $line.Length)
    $rows += $line
}
Save-PixelArt -Rows $rows -Palette $pal -Path $out
if ($Preview) { Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $root 'tools/textures/preview/levitate_ponder_particles.png') -Scale 8 }
Write-Host 'ponder textures written'
