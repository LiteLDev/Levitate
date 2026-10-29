# Textures of the stock unit (U26): stock link, stock ticker, factory gauge, redstone requester,
# shopping list. Original pixel art.
#
#   pwsh tools/textures/stock.ps1            # writes assets/pack/rp/textures/{blocks,items}/
#   pwsh tools/textures/stock.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$blocks = Join-Path $root 'assets/pack/rp/textures/blocks'
$items = Join-Path $root 'assets/pack/rp/textures/items'

function New-Canvas([char]$fill = '.') { , (0..15 | ForEach-Object { , ([char[]]((New-Object string $fill, 16))) }) }
function Set-Px($c, $x, $y, [char]$k) { if ($x -ge 0 -and $x -lt 16 -and $y -ge 0 -and $y -lt 16) { $c[$y][$x] = $k } }
function Fill($c, $x0, $y0, $x1, $y1, [char]$k) { for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y $k } } }
function Rows($c) { , ($c | ForEach-Object { -join $_ }) }
function Save($c, $pal, $path) {
    $rows = Rows $c
    Save-PixelArt -Rows $rows -Palette $pal -Path $path
    if ($Preview) { Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $root "tools/textures/preview/$(Split-Path -Leaf $path)") -Scale 8 }
}
# a bevelled frame: light top/left, dark bottom/right
function Frame($c, $x0, $y0, $x1, $y1, [char]$light, [char]$dark) {
    Fill $c $x0 $y0 $x1 $y0 $light; Fill $c $x0 $y0 $x0 $y1 $light
    Fill $c $x0 $y1 $x1 $y1 $dark; Fill $c $x1 $y0 $x1 $y1 $dark
}
# speckled fill (deterministic)
function Speckle($c, $x0, $y0, $x1, $y1, [char]$base, [char]$spot, [int]$mod) {
    for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) {
        Set-Px $c $x $y ($(if ((($x * 5 + $y * 3) % $mod) -eq 0) { $spot } else { $base }))
    } }
}

$brass = @{ b = '#d9a441'; h = '#f2cf6b'; d = '#9c6d22'; q = '#6e4a14'; k = '#3b2a12' }
$wood = @{ w = '#9b7547'; l = '#b8935e'; v = '#6f5130'; u = '#4d3720' }
$extra = @{ g = '#55c2d9'; y = '#a8ecf7'; r = '#c23a2e'; o = '#ff6a4f'; e = '#3a3a3a'; m = '#5a5a5a'; p = '#efe7d2'; a = '#c9bea0'; s = '#2b2b2b' }
$pal = @{}; foreach ($h in @($brass, $wood, $extra)) { foreach ($k in $h.Keys) { $pal[$k] = $h[$k] } }

# --- stock link: brass plate with a grid of vents ---
$c = New-Canvas 'b'; Speckle $c 1 1 14 14 'b' 'h' 9; Frame $c 0 0 15 15 'h' 'q'
foreach ($i in 3, 6, 9, 12) { Fill $c 3 $i 12 $i 'd' }
Save $c $pal (Join-Path $blocks 'stock_link.png')
$c = New-Canvas 'w'; Speckle $c 0 0 15 15 'w' 'l' 7; Frame $c 0 0 15 15 'l' 'u'; Fill $c 2 6 13 9 'd'; Fill $c 3 7 12 8 'h'
Save $c $pal (Join-Path $blocks 'stock_link_side.png')
$c = New-Canvas 'g'; Fill $c 0 0 15 15 'g'; Fill $c 4 4 11 11 'y'; Frame $c 0 0 15 15 'y' 'e'
Save $c $pal (Join-Path $blocks 'stock_link_bulb.png')
$c = New-Canvas 'm'; Fill $c 4 4 11 11 'e'; Frame $c 0 0 15 15 'm' 's'
Save $c $pal (Join-Path $blocks 'stock_link_bulb_off.png')

# --- stock ticker: wooden case, glass screen with a stock list, brass keys ---
$c = New-Canvas 'w'; Speckle $c 0 0 15 15 'w' 'l' 6; Frame $c 0 0 15 15 'l' 'u'
foreach ($y in 4, 9, 14) { Fill $c 1 $y 14 $y 'v' }
Save $c $pal (Join-Path $blocks 'stock_ticker_side.png')
$c = New-Canvas 'e'; Frame $c 0 0 15 15 'h' 'q'; Fill $c 2 2 13 13 's'
foreach ($y in 3, 6, 9, 12) { Fill $c 3 $y 6 $y 'g'; Fill $c 8 $y 12 $y 'y' }
Save $c $pal (Join-Path $blocks 'stock_ticker_screen.png')
$c = New-Canvas 'q'; for ($y = 1; $y -lt 16; $y += 3) { for ($x = 1; $x -lt 16; $x += 3) { Fill $c $x $y ($x + 1) ($y + 1) 'b'; Set-Px $c $x $y 'h' } }
Save $c $pal (Join-Path $blocks 'stock_ticker_keys.png')

# --- factory gauge: a light panel with a brass rim; the dial ---
$c = New-Canvas 'p'; Speckle $c 1 1 14 14 'p' 'a' 11; Frame $c 0 0 15 15 'h' 'q'
Save $c $pal (Join-Path $blocks 'factory_gauge.png')
$c = New-Canvas 'p'; Frame $c 0 0 15 15 'k' 'k'; Fill $c 7 3 8 8 'r'; Fill $c 6 7 9 9 'q'
foreach ($p in @(@(3, 4), @(12, 4), @(2, 9), @(13, 9))) { Set-Px $c $p[0] $p[1] 'k' }
Save $c $pal (Join-Path $blocks 'factory_gauge_dial.png')

# --- redstone requester: wooden casing, a brass slot front, redstone top ---
$c = New-Canvas 'w'; Speckle $c 0 0 15 15 'w' 'l' 5; Frame $c 0 0 15 15 'l' 'u'; Fill $c 0 0 15 2 'q'; Fill $c 0 13 15 15 'q'
Save $c $pal (Join-Path $blocks 'redstone_requester_side.png')
$c = New-Canvas 'w'; Speckle $c 0 0 15 15 'w' 'l' 5; Frame $c 0 0 15 15 'l' 'u'; Fill $c 3 4 12 11 'q'; Fill $c 4 5 11 10 'k'; Fill $c 5 7 10 8 'b'
Save $c $pal (Join-Path $blocks 'redstone_requester_front.png')
$c = New-Canvas 'b'; Frame $c 0 0 15 15 'h' 'q'; Fill $c 5 5 10 10 'r'; Fill $c 6 6 9 9 'r'; Frame $c 4 4 11 11 'q' 'q'
Save $c $pal (Join-Path $blocks 'redstone_requester_top.png')
$c = New-Canvas 'b'; Frame $c 0 0 15 15 'h' 'q'; Fill $c 5 5 10 10 'o'; Fill $c 6 6 9 9 'o'; Frame $c 4 4 11 11 'q' 'q'
Save $c $pal (Join-Path $blocks 'redstone_requester_top_powered.png')
$c = New-Canvas 'v'; Speckle $c 0 0 15 15 'v' 'u' 4
Save $c $pal (Join-Path $blocks 'redstone_requester_bottom.png')

# --- shopping list: a paper slip with lines and a brass clip ---
$c = New-Canvas '.'; Fill $c 3 2 12 14 'p'; Frame $c 3 2 12 14 'p' 'a'
foreach ($y in 5, 7, 9, 11) { Fill $c 5 $y 10 $y 'm' }
Fill $c 6 1 9 3 'b'; Set-Px $c 6 1 'h'; Set-Px $c 9 3 'q'; Fill $c 9 12 11 12 'r'
Save $c $pal (Join-Path $items 'shopping_list.png')
Write-Host 'stock textures written'
