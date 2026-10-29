# Textures of the U24 `display` unit: display link, display board, nixie tubes (16 filament
# colours), clipboard (block + item). Original pixel art (not derived from Create's assets).
#
#   pwsh tools/textures/display.ps1            # writes assets/pack/rp/textures/{blocks,items}/*.png
#   pwsh tools/textures/display.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$blocks = Join-Path $PSScriptRoot '../../assets/pack/rp/textures/blocks'
$items  = Join-Path $PSScriptRoot '../../assets/pack/rp/textures/items'
New-Item -ItemType Directory -Force $blocks, $items | Out-Null
$previewDir = Join-Path $PSScriptRoot 'preview'

function Out-Tex([string[]]$Rows, [hashtable]$Pal, [string]$Dir, [string]$Name) {
    Save-PixelArt -Rows $Rows -Palette $Pal -Path (Join-Path $Dir "$Name.png")
    if ($Preview) {
        New-Item -ItemType Directory -Force $previewDir | Out-Null
        Save-PixelArt -Rows $Rows -Palette $Pal -Path (Join-Path $previewDir "$Name.png") -Scale 8
    }
}

# Builds 16 rows from a function (x, y) -> palette key.
function New-Rows([scriptblock]$Fn) {
    $rows = for ($y = 0; $y -lt 16; $y++) { -join (0..15 | ForEach-Object { & $Fn $_ $y }) }
    return , $rows
}

# ---- display link: brass plate with a riveted rim, a dark lens that lights up when powered ----
$brass = @{ o = '#4a3014'; a = '#f2cf5c'; b = '#d29d36'; c = '#a8741f'; r = '#fff1a8' }
$plate = New-Rows {
    param($x, $y)
    if ($x -eq 0 -or $y -eq 0 -or $x -eq 15 -or $y -eq 15) { 'o' }
    elseif (($x -eq 2 -or $x -eq 13) -and ($y -eq 2 -or $y -eq 13)) { 'r' }
    elseif ($x -eq 1 -or $y -eq 1) { 'a' }
    elseif ($x -eq 14 -or $y -eq 14) { 'c' }
    else { 'b' }
}
Out-Tex $plate $brass $blocks 'display_link'
$side = New-Rows {
    param($x, $y)
    if ($y -eq 0 -or $y -eq 15) { 'o' } elseif ($y % 4 -eq 1) { 'a' } elseif ($y % 4 -eq 3) { 'c' } else { 'b' }
}
Out-Tex $side $brass $blocks 'display_link_side'
function Lens([string]$Glow, [string]$Mid, [string]$Dark) {
    $rows = New-Rows {
        param($x, $y)
        $dx = $x - 7.5; $dy = $y - 7.5; $d = [Math]::Sqrt($dx * $dx + $dy * $dy)
        if ($d -gt 7.2) { 'o' } elseif ($d -gt 6) { 'c' } elseif ($d -lt 2.2) { 'g' } elseif ($d -lt 4.5) { 'm' } else { 'd' }
    }
    return @{ rows = $rows; pal = @{ o = '#4a3014'; c = '#a8741f'; g = $Glow; m = $Mid; d = $Dark } }
}
$off = Lens '#5b6b8a' '#38425a' '#1f2433'
Out-Tex $off.rows $off.pal $blocks 'display_link_lens'
$on = Lens '#e9fbff' '#8fe0ff' '#3a8fc4'
Out-Tex $on.rows $on.pal $blocks 'display_link_powered'

# ---- display board: dark flaps in two rows of four, a metal frame with a shaft hole ------------
$flaps = New-Rows {
    param($x, $y)
    if ($x -eq 0 -or $x -eq 15 -or $y -eq 0 -or $y -eq 15) { 'f' }
    elseif ($y -eq 7 -or $y -eq 8) { 'f' }
    elseif (($x - 1) % 4 -eq 3) { 's' }
    elseif ($y -eq 4 -or $y -eq 11) { 'l' }
    else { 'k' }
}
Out-Tex $flaps @{ f = '#3c3f44'; s = '#22252a'; l = '#3a3d42'; k = '#2c2f34' } $blocks 'display_board_front'
$metal = @{ o = '#2b2b30'; a = '#a6a6b0'; b = '#8a8a96'; c = '#6c6c78'; h = '#3a3a42'; w = '#c8a46a' }
$boardSide = New-Rows {
    param($x, $y)
    $dx = $x - 7.5; $dy = $y - 7.5
    if ($x -eq 0 -or $y -eq 0 -or $x -eq 15 -or $y -eq 15) { 'o' }
    elseif ([Math]::Abs($dx) -lt 2 -and [Math]::Abs($dy) -lt 2) { 'w' }
    elseif ([Math]::Abs($dx) -lt 3 -and [Math]::Abs($dy) -lt 3) { 'h' }
    elseif ($x -eq 1 -or $y -eq 1) { 'a' } elseif ($x -eq 14 -or $y -eq 14) { 'c' } else { 'b' }
}
Out-Tex $boardSide $metal $blocks 'display_board_side'
$boardBack = New-Rows {
    param($x, $y)
    if ($x -eq 0 -or $y -eq 0 -or $x -eq 15 -or $y -eq 15) { 'o' } elseif (($x + $y) % 7 -eq 0) { 'a' } elseif ($y % 5 -eq 2) { 'c' } else { 'b' }
}
Out-Tex $boardBack $metal $blocks 'display_board_back'

# ---- nixie tubes: dark socket, faint glass, a glowing filament per dye colour ------------------
$socket = New-Rows {
    param($x, $y)
    if ($x -eq 0 -or $y -eq 0 -or $x -eq 15 -or $y -eq 15) { 'o' } elseif (($x + 2 * $y) % 6 -eq 0) { 'a' } else { 'b' }
}
Out-Tex $socket @{ o = '#1c1a18'; a = '#5b4b3a'; b = '#3a3026' } $blocks 'nixie_tube'
$glass = New-Rows {
    param($x, $y)
    if ($x -eq 0 -or $x -eq 15 -or $y -eq 0) { 'e' } elseif ($x -eq 2 -and $y -lt 12) { 'h' } else { 'g' }
}
Out-Tex $glass @{ e = '#80d8e8f0'; h = '#90ffffff'; g = '#30c8e0f0' } $blocks 'nixie_tube_glass'
$dyes = [ordered]@{
    white = '#f9fffe'; orange = '#ff9a3c'; magenta = '#e070d8'; light_blue = '#6fd0ff'; yellow = '#ffe46a'; lime = '#a8f04a'
    pink = '#ffa8c4'; gray = '#8a9296'; light_gray = '#c8c8c2'; cyan = '#3ad8d8'; purple = '#b06af0'; blue = '#6a74ff'
    brown = '#c08050'; green = '#7cb030'; red = '#ff5040'; black = '#5a5a66'
}
foreach ($d in $dyes.Keys) {
    $c = $dyes[$d]
    $fil = New-Rows { param($x, $y) if (($x + $y) % 3 -eq 0) { 'h' } else { 'c' } }
    Out-Tex $fil @{ c = (Shade-Color $c 1.0); h = (Shade-Color $c 1.35) } $blocks "nixie_tube_filament_$d"
}

# ---- clipboard: a wooden board, paper with ruled lines (written: with scribbles), item icon ----
$wood = New-Rows { param($x, $y) if ($x -eq 0 -or $x -eq 15 -or $y -eq 0 -or $y -eq 15) { 'o' } elseif (($y * 3 + $x) % 7 -eq 0) { 'd' } else { 'w' } }
Out-Tex $wood @{ o = '#4a3018'; d = '#7a5530'; w = '#9b6e3e' } $blocks 'clipboard_board'
$paper = New-Rows {
    param($x, $y)
    if ($x -eq 0 -or $x -eq 15 -or $y -eq 0 -or $y -eq 15) { 'e' } elseif ($y % 3 -eq 2 -and $x -gt 1 -and $x -lt 14) { 'l' } else { 'p' }
}
Out-Tex $paper @{ e = '#c9c2b0'; l = '#b8c8dc'; p = '#f4f0e6' } $blocks 'clipboard_paper'
$written = New-Rows {
    param($x, $y)
    if ($x -eq 0 -or $x -eq 15 -or $y -eq 0 -or $y -eq 15) { 'e' }
    elseif ($y % 3 -eq 1 -and $x -gt 1 -and $x -lt (12 - ($y % 5))) { 'k' }
    elseif ($y % 3 -eq 2 -and $x -gt 1 -and $x -lt 14) { 'l' } else { 'p' }
}
Out-Tex $written @{ e = '#c9c2b0'; l = '#b8c8dc'; p = '#f4f0e6'; k = '#3a3a52' } $blocks 'clipboard_paper_written'
$icon = @(
    '................',
    '......cccc......',
    '...oooc..cooo...',
    '..owwwccccwwwo..',
    '..owppppppppwo..',
    '..owpkkkkkppwo..',
    '..owppppppppwo..',
    '..owpkkkkppppo..',
    '..owppppppppwo..',
    '..owpkkkkkkpwo..',
    '..owppppppppwo..',
    '..owpkkkppppwo..',
    '..owppppppppwo..',
    '..owwwwwwwwwwo..',
    '...oooooooooo...',
    '................'
)
Out-Tex $icon @{ o = '#3a2410'; w = '#9b6e3e'; p = '#f4f0e6'; k = '#5a6478'; c = '#a6a6b0' } $items 'clipboard'

Write-Host "display textures written"
