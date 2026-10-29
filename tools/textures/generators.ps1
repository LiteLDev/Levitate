# Textures of the generators unit (U7): hand crank, copper / dyed valve handles, water wheels (one
# per wood type plus the stacked sheet the client mod spins) and the creative motor. Original pixel
# art drawn here (not derived from Create's assets).
#
# Every texture is a 32x32 atlas of four 16x16 regions that the models in tools/models/generators.ps1
# map their faces onto:
#   (0,0) main material   (16,0) second material   (0,16) third material   (16,16) shaft (andesite)
#
#   pwsh tools/textures/generators.ps1            # writes assets/pack/rp/textures/blocks/
#   pwsh tools/textures/generators.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$blocks = Join-Path $root 'assets/pack/rp/textures/blocks'
New-Item -ItemType Directory -Force $blocks | Out-Null

# --- canvas helpers (char grids of any size) -----------------------------------------------------
function New-Canvas([int]$w = 32, [int]$h = 32, [char]$fill = '.') { , (0..($h - 1) | ForEach-Object { , ([char[]]((New-Object string $fill, $w))) }) }
function Set-Px($c, $x, $y, [char]$k) { if ($y -ge 0 -and $y -lt $c.Count -and $x -ge 0 -and $x -lt $c[0].Count) { $c[$y][$x] = $k } }
function Fill($c, $x0, $y0, $x1, $y1, [char]$k) { for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y $k } } }
function Rows($c) { , ($c | ForEach-Object { -join $_ }) }
function Save($c, $pal, $path) {
    $rows = Rows $c
    Save-PixelArt -Rows $rows -Palette $pal -Path $path
    if ($Preview) { Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $root "tools/textures/preview/$(Split-Path -Leaf $path)") -Scale 8 }
}
function Merge-Pal($base, $over) { $h = @{}; foreach ($k in $base.Keys) { $h[$k] = $base[$k] }; foreach ($k in $over.Keys) { $h[$k] = $over[$k] }; $h }

# --- regions ------------------------------------------------------------------------------------------
# planks: 4-px boards with dark seams, staggered butt joints and grain flecks (keys p l d g)
function Planks($c, $ox, $oy) {
    for ($y = 0; $y -lt 16; $y++) {
        $board = [math]::Floor($y / 4)
        for ($x = 0; $x -lt 16; $x++) {
            $k = if ($y % 4 -eq 3) { 'd' }
            elseif ((($x + $board * 6) % 16) -eq 0) { 'g' }
            elseif ((($x * 5 + $y * 3 + $board) % 9) -eq 0) { 'l' }
            elseif ((($x * 3 + $y * 7) % 13) -eq 0) { 'g' }
            else { 'p' }
            Set-Px $c ($ox + $x) ($oy + $y) $k
        }
    }
}
# log side: vertical bark ridges (keys b v h)
function LogSide($c, $ox, $oy) {
    for ($y = 0; $y -lt 16; $y++) {
        for ($x = 0; $x -lt 16; $x++) {
            $wobble = [math]::Floor(($y + $x * 2) / 5) % 2
            $k = if ((($x + $wobble) % 4) -eq 0) { 'v' } elseif ((($x * 7 + $y * 5) % 11) -eq 0) { 'h' } else { 'b' }
            Set-Px $c ($ox + $x) ($oy + $y) $k
        }
    }
}
# log top: bark rim and growth rings (keys b r u c)
function LogTop($c, $ox, $oy) {
    for ($y = 0; $y -lt 16; $y++) {
        for ($x = 0; $x -lt 16; $x++) {
            $edge = [math]::Min([math]::Min($x, 15 - $x), [math]::Min($y, 15 - $y))
            $d = [math]::Max([math]::Abs($x - 7.5), [math]::Abs($y - 7.5))
            $k = if ($edge -eq 0) { 'b' } elseif ([math]::Floor($d) % 3 -eq 0) { 'u' } elseif ($d -lt 1.5) { 'c' } else { 'r' }
            Set-Px $c ($ox + $x) ($oy + $y) $k
        }
    }
}
# andesite shaft: lengthwise bands with a bright edge (keys a s e k)
function Shaft($c, $ox, $oy) {
    for ($y = 0; $y -lt 16; $y++) {
        for ($x = 0; $x -lt 16; $x++) {
            $band = $x % 4
            $k = if ($band -eq 0) { 'e' } elseif ($band -eq 3) { 'k' } elseif ((($x + $y * 3) % 7) -eq 0) { 's' } else { 'a' }
            Set-Px $c ($ox + $x) ($oy + $y) $k
        }
    }
}
# brushed metal with a rim (keys m i n o: base, light, shadow, outline)
function Metal($c, $ox, $oy) {
    for ($y = 0; $y -lt 16; $y++) {
        for ($x = 0; $x -lt 16; $x++) {
            $k = if ($x -eq 0 -or $y -eq 0) { 'i' } elseif ($x -eq 15 -or $y -eq 15) { 'o' } elseif ((($x * 3 + $y) % 6) -eq 0) { 'i' } elseif ((($x + $y * 5) % 9) -eq 0) { 'n' } else { 'm' }
            Set-Px $c ($ox + $x) ($oy + $y) $k
        }
    }
}
# darker metal for hubs and spokes (keys q x w)
function DarkMetal($c, $ox, $oy) {
    for ($y = 0; $y -lt 16; $y++) {
        for ($x = 0; $x -lt 16; $x++) {
            $k = if ((($x + $y) % 5) -eq 0) { 'x' } elseif ((($x * 2 + $y * 3) % 7) -eq 0) { 'w' } else { 'q' }
            Set-Px $c ($ox + $x) ($oy + $y) $k
        }
    }
}

$shaftPal = @{ a = '#9a9d96'; s = '#80837c'; e = '#c3c6be'; k = '#5f625c' }

# --- water wheels: one texture per wood type -------------------------------------------------------
$woods = [ordered]@{
    oak      = @{ p = '#a68352'; l = '#bb9662'; d = '#6f5534'; g = '#86683f'; b = '#6b5233'; v = '#4a3820'; h = '#82674a'; r = '#b0915c'; u = '#8a6c41'; c = '#c4a46d' }
    spruce   = @{ p = '#735433'; l = '#86643f'; d = '#4a3420'; g = '#5d4329'; b = '#3d2a17'; v = '#271a0d'; h = '#554029'; r = '#7a5a36'; u = '#5c4228'; c = '#8f6c43' }
    birch    = @{ p = '#c5b47b'; l = '#d6c78e'; d = '#9a8a58'; g = '#ae9e68'; b = '#dddcd5'; v = '#3b3a35'; h = '#f0efe9'; r = '#d2c28a'; u = '#b8a670'; c = '#e2d49e' }
    jungle   = @{ p = '#a37552'; l = '#b88865'; d = '#6e4d34'; g = '#8a6143'; b = '#5a4a1d'; v = '#3c3112'; h = '#76622b'; r = '#ad7e57'; u = '#8d6343'; c = '#c29169' }
    acacia   = @{ p = '#ae5f34'; l = '#c4713f'; d = '#743d1f'; g = '#8f4c29'; b = '#6b655b'; v = '#47433c'; h = '#868073'; r = '#b8683a'; u = '#94512d'; c = '#cc7c47' }
    dark_oak = @{ p = '#4d321a'; l = '#5e3f22'; d = '#2e1e0e'; g = '#3d2814'; b = '#3f3020'; v = '#251b10'; h = '#56432e'; r = '#55381d'; u = '#3f2914'; c = '#6a4a28' }
    mangrove = @{ p = '#783a35'; l = '#8c4741'; d = '#4d2421'; g = '#622e2a'; b = '#56392e'; v = '#36231c'; h = '#6d4c3e'; r = '#83413b'; u = '#66302c'; c = '#995048' }
    cherry   = @{ p = '#e2b6ae'; l = '#ecc8c1'; d = '#b98a83'; g = '#cd9f97'; b = '#3d212a'; v = '#24121a'; h = '#5a3540'; r = '#e6bdb5'; u = '#cfa197'; c = '#f2d2cb' }
    bamboo   = @{ p = '#c7b24f'; l = '#d8c562'; d = '#93812f'; g = '#ae9b3f'; b = '#7a8c38'; v = '#566425'; h = '#9bae4b'; r = '#cdb957'; u = '#ad9a40'; c = '#e0cd6c' }
    crimson  = @{ p = '#6c3550'; l = '#7f4161'; d = '#432033'; g = '#572a41'; b = '#5d1b25'; v = '#3a0f17'; h = '#8a2b36'; r = '#753a57'; u = '#582b42'; c = '#8c4b6b' }
    warped   = @{ p = '#2d6c66'; l = '#378079'; d = '#1b4440'; g = '#245752'; b = '#3a3a52'; v = '#232336'; h = '#2f8c79'; r = '#317570'; u = '#255954'; c = '#3f8f87' }
    pale_oak = @{ p = '#e3d8d4'; l = '#efe7e3'; d = '#b9aca7'; g = '#cfc3be'; b = '#5d564d'; v = '#3f3a33'; h = '#78706a'; r = '#e8dfdb'; u = '#cbbfba'; c = '#f3ecea' }
}
$wheelFiles = @()
foreach ($wood in $woods.Keys) {
    $c = New-Canvas
    Planks $c 0 0
    LogSide $c 16 0
    LogTop $c 0 16
    Shaft $c 16 16
    $path = "$blocks/water_wheel_$wood.png"
    Save $c (Merge-Pal $woods[$wood] $shaftPal) $path
    $wheelFiles += $path
}
# the client mod's sheet: the 12 wood textures stacked (bones wheel_<n> map onto band n)
$sheet = New-Object System.Drawing.Bitmap 32, (32 * $wheelFiles.Count), ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($sheet)
try {
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    for ($i = 0; $i -lt $wheelFiles.Count; $i++) {
        $img = [System.Drawing.Image]::FromFile($wheelFiles[$i])
        try { $g.DrawImageUnscaled($img, 0, 32 * $i) } finally { $img.Dispose() }
    }
    $sheet.Save("$blocks/water_wheel_rotor.png", [System.Drawing.Imaging.ImageFormat]::Png)
} finally { $g.Dispose(); $sheet.Dispose() }

# --- hand crank: oak arm, andesite casing hub, dark grip --------------------------------------------
# layout: arm wood (0,0), grip (16,0), hub metal (0,16), shaft (16,16)
$c = New-Canvas
Planks $c 0 0
DarkMetal $c 16 0
Metal $c 0 16
Shaft $c 16 16
$crankPal = Merge-Pal (Merge-Pal $woods['oak'] $shaftPal) @{ m = '#8e918a'; i = '#b5b8b0'; n = '#6d7069'; o = '#4f524c'; q = '#4a3626'; x = '#3a2a1c'; w = '#5c4532' }
Save $c $crankPal "$blocks/hand_crank.png"

# --- valve handles: copper and the 16 dye colours -------------------------------------------------
$dyes = [ordered]@{
    copper = '#c46f4a'; white = '#e9ecec'; orange = '#f07613'; magenta = '#bd44b3'; light_blue = '#3aafd9'
    yellow = '#f8c527'; lime = '#70b919'; pink = '#ed8dac'; gray = '#3e4447'; light_gray = '#8e8e86'
    cyan = '#158991'; purple = '#792aac'; blue = '#35399d'; brown = '#724728'; green = '#546d1b'; red = '#a12722'; black = '#141519'
}
foreach ($name in $dyes.Keys) {
    $base = $dyes[$name]
    $c = New-Canvas
    Metal $c 0 0
    DarkMetal $c 16 0
    DarkMetal $c 0 16
    Shaft $c 16 16
    $pal = Merge-Pal $shaftPal @{
        m = $base; i = (Shade-Color $base 1.35); n = (Shade-Color $base 0.8); o = (Shade-Color $base 0.6)
        q = (Shade-Color $base 0.55); x = (Shade-Color $base 0.42); w = (Shade-Color $base 0.7)
    }
    if ($name -eq 'copper') { $pal = Merge-Pal $pal @{ i = '#e7a07a'; w = '#5d9a83' } } # a hint of patina
    Save $c $pal "$blocks/$($name)_valve_handle.png"
}

# --- creative motor: violet casing, dark trim, gold coils -----------------------------------------
$c = New-Canvas
Metal $c 0 0
DarkMetal $c 16 0
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        Set-Px $c $x (16 + $y) ($(if ($y % 3 -eq 0) { 'y' } elseif ((($x + $y) % 4) -eq 0) { 'j' } else { 'z' }))
    } }
Shaft $c 16 16
$motorPal = Merge-Pal $shaftPal @{ m = '#8a5fb8'; i = '#b48ae0'; n = '#6b4592'; o = '#4a2f68'; q = '#2f2440'; x = '#211a2e'; w = '#463760'; y = '#e3b54d'; j = '#fff0a8'; z = '#b8862f' }
Save $c $motorPal "$blocks/creative_motor.png"

Write-Host 'generators textures written'
