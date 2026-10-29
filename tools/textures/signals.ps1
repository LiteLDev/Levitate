# Textures of the signals unit (U28): train signal (body + lamp heads per type and state) and
# track observer. Original pixel art.
#
#   pwsh tools/textures/signals.ps1            # writes assets/pack/rp/textures/blocks/
#   pwsh tools/textures/signals.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$blocks = Join-Path $root 'assets/pack/rp/textures/blocks'

function New-Canvas([char]$fill = '.') { , (0..15 | ForEach-Object { , ([char[]]((New-Object string $fill, 16))) }) }
function Set-Px($c, $x, $y, [char]$k) { if ($x -ge 0 -and $x -lt 16 -and $y -ge 0 -and $y -lt 16) { $c[$y][$x] = $k } }
function Fill($c, $x0, $y0, $x1, $y1, [char]$k) { for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y $k } } }
function Save($c, $pal, $name) {
    $rows = , ($c | ForEach-Object { -join $_ })
    Save-PixelArt -Rows $rows[0] -Palette $pal -Path (Join-Path $blocks "$name.png")
    if ($Preview) { Save-PixelArt -Rows $rows[0] -Palette $pal -Path (Join-Path $root "tools/textures/preview/$name.png") -Scale 8 }
}

# dark railway-casing metal with brass rivets
# (PowerShell hashtables ignore case: every palette letter is distinct regardless of case)
$metal = @{ m = '#3b3a40'; n = '#4c4b52'; h = '#64636b'; s = '#2a292e'; r = '#b38a3c'; q = '#e0b862' }

# body: base plate and post
$c = New-Canvas 'm'
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { if ((($x * 5 + $y * 3) % 7) -eq 0) { Set-Px $c $x $y 'n' } } }
Fill $c 0 0 15 0 'h'; Fill $c 0 0 0 15 'h'; Fill $c 0 15 15 15 's'; Fill $c 15 0 15 15 's'
foreach ($p in @(@(2, 2), @(13, 2), @(2, 13), @(13, 13))) { Set-Px $c $p[0] $p[1] 'q'; Set-Px $c ($p[0] + 1) ($p[1] + 1) 'r' }
Save $c $metal 'track_signal_body'

# lamp heads: the head side uses x 3..12, y 4..10 of the texture (10 x 7 px)
$lamps = @{
    off    = @{ a = '#2e2f2c'; b = '#454640'; c = '#5a5b55' }
    red    = @{ a = '#e2382a'; b = '#9e1d14'; c = '#ffb3a6' }
    yellow = @{ a = '#f2c330'; b = '#a87d10'; c = '#fff0b0' }
    green  = @{ a = '#3fd25a'; b = '#1d8a33'; c = '#c4ffcc' }
}
foreach ($type in 'entry', 'cross') {
    foreach ($state in $lamps.Keys) {
        $c = New-Canvas 'm'
        Fill $c 3 4 12 10 's'          # housing
        Fill $c 4 4 11 4 'h'           # visor
        Fill $c 3 4 3 10 'n'; Fill $c 12 4 12 10 'n'
        # round lamp in the middle (6 x 5)
        Fill $c 6 6 9 9 'a'; Fill $c 5 7 10 8 'a'
        Set-Px $c 6 6 'c'; Set-Px $c 7 6 'c'
        Fill $c 6 9 9 9 'b'
        if ($type -eq 'cross') {
            # white diagonal bars on both sides of the lamp: Create's cross signal head
            foreach ($i in 0..2) { Set-Px $c (4 + $i % 2) (5 + $i * 2) 'w'; Set-Px $c (11 - $i % 2) (5 + $i * 2) 'w' }
            Set-Px $c 4 5 'w'; Set-Px $c 11 5 'w'; Set-Px $c 4 9 'w'; Set-Px $c 11 9 'w'
        }
        $pal = @{} + $metal + $lamps[$state] + @{ w = '#e8e8e0' }
        Save $c $pal "track_signal_lamp_${type}_$state"
    }
}

# observer: casing sides, a lens on top that lights up while a train passes
$c = New-Canvas 'm'
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { if ((($x * 3 + $y * 5) % 9) -eq 0) { Set-Px $c $x $y 'n' } } }
Fill $c 0 0 15 0 'h'; Fill $c 0 0 0 15 'h'; Fill $c 0 15 15 15 's'; Fill $c 15 0 15 15 's'
Fill $c 2 6 13 9 's'; Fill $c 3 7 12 8 'r'
Save $c $metal 'track_observer_side'
foreach ($on in $false, $true) {
    $c = New-Canvas 'm'
    Fill $c 0 0 15 0 'h'; Fill $c 0 0 0 15 'h'; Fill $c 0 15 15 15 's'; Fill $c 15 0 15 15 's'
    Fill $c 3 3 12 12 's'
    Fill $c 5 5 10 10 'o'; Fill $c 4 6 11 9 'o'; Fill $c 6 4 9 11 'o'
    Fill $c 6 6 9 9 'i'; Set-Px $c 6 6 'c'
    foreach ($p in @(@(1, 1), @(14, 1), @(1, 14), @(14, 14))) { Set-Px $c $p[0] $p[1] 'q' }
    $eye = if ($on) { @{ o = '#d0402f'; i = '#ff7a4a'; c = '#ffd0b0' } } else { @{ o = '#4a2c2a'; i = '#6b3a35'; c = '#8a5a50' } }
    $pal = @{} + $metal + $eye
    Save $c $pal $(if ($on) { 'track_observer_top_on' } else { 'track_observer_top' })
}
Write-Host 'signals textures written'
