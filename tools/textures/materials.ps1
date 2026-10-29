# Textures of the materials unit (U9): every material item, food, metal / ore block and the
# decorative stone palettes. Original pixel art (procedural shapes + hand-made 16x16 templates).
#
#   pwsh tools/textures/materials.ps1            # writes assets/pack/rp/textures/{blocks,items}/
#   pwsh tools/textures/materials.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$blocks = Join-Path $root 'assets/pack/rp/textures/blocks'
$items = Join-Path $root 'assets/pack/rp/textures/items'

# ---- helpers --------------------------------------------------------------------------------------
function New-Canvas([char]$fill = '.') { , (0..15 | ForEach-Object { , ([char[]]((New-Object string $fill, 16))) }) }
function Set-Px($c, $x, $y, [char]$k) { if ($x -ge 0 -and $x -lt 16 -and $y -ge 0 -and $y -lt 16) { $c[$y][$x] = $k } }
function Fill($c, $x0, $y0, $x1, $y1, [char]$k) { for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y $k } } }
function Rows($c) { , ($c | ForEach-Object { -join $_ }) }
function Save-Rows([string[]]$rows, $pal, $path) {
    # hand-drawn templates: fit every row to 16 pixels (transparent padding)
    $rows = @($rows | ForEach-Object { if ($_.Length -gt 16) { $_.Substring(0, 16) } else { $_.PadRight(16, '.') } })
    while ($rows.Count -lt 16) { $rows += '................' }
    Save-PixelArt -Rows $rows -Palette $pal -Path $path
    if ($Preview) { Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $root "tools/textures/preview/$(Split-Path -Leaf $path)") -Scale 8 }
}
function Save($c, $pal, $path) { Save-Rows (Rows $c) $pal $path }
# deterministic hash noise 0..1
function N([int]$x, [int]$y, [int]$seed) {
    [long]$h = ($x * 374761393L + $y * 668265263L + $seed * 2147483647L) -band 0xFFFFFFFFL
    $h = (($h -bxor ($h -shr 13)) * 1274126177L) -band 0xFFFFFFFFL
    $h = $h -bxor ($h -shr 16)
    ($h -band 0xFFFF) / 65535.0
}
# 6-step ramp around a base colour: 1 darkest .. 6 lightest
function Ramp($base) {
    Pal '1' (Shade-Color $base 0.5) '2' (Shade-Color $base 0.68) '3' (Shade-Color $base 0.84) '4' (ConvertTo-Color $base) '5' (Shade-Color $base 1.14) '6' (Shade-Color $base 1.3)
}
function Pal { $h = [hashtable]::new([StringComparer]::Ordinal); for ($i = 0; $i -lt $args.Count; $i += 2) { $h[[string]$args[$i]] = $args[$i + 1] }; $h }
function Merge-Pal($a, $b) { $h = [hashtable]::new([StringComparer]::Ordinal); foreach ($k in $a.Keys) { $h[$k] = $a[$k] }; foreach ($k in $b.Keys) { $h[$k] = $b[$k] }; $h }
# noisy fill with ramp keys 3/4/5
function Noise-Fill($c, $x0, $y0, $x1, $y1, $seed, [double]$dark = 0.22, [double]$light = 0.8) {
    for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) {
        $v = N $x $y $seed
        Set-Px $c $x $y ($(if ($v -lt $dark) { '3' } elseif ($v -gt $light) { '5' } else { '4' }))
    } }
}
# bevelled rectangle: light top/left edge, dark bottom/right edge
function Bevel($c, $x0, $y0, $x1, $y1, [char]$hi = '5', [char]$lo = '2') {
    for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y0 $hi; Set-Px $c $x $y1 $lo }
    for ($y = $y0; $y -le $y1; $y++) { Set-Px $c $x0 $y $hi; Set-Px $c $x1 $y $lo }
}

# ---- decorative stone palettes ------------------------------------------------------------------
$stoneColors = [ordered]@{ granite = '#9b6b56'; diorite = '#c4c3c0'; andesite = '#888a8b'; calcite = '#dcdbd4'; dripstone = '#866b5d'; deepslate = '#55555a'; tuff = '#6e6f66'; asurine = '#4d6bb5'; crimsite = '#a63f3c'; limestone = '#c8bf9f'; ochrum = '#c49a52'; scoria = '#634532'; scorchia = '#3d3836'; veridium = '#2f7c74' }
$i = 0
foreach ($n in $stoneColors.Keys) {
    $i++; $seed = $i * 97
    $pal = Ramp $stoneColors[$n]
    $dir = Join-Path $blocks 'palettes'

    # cut: one large bevelled slab with fine grain
    $c = New-Canvas; Noise-Fill $c 0 0 15 15 $seed; Bevel $c 0 0 15 15 '5' '2'; Bevel $c 1 1 14 14 '4' '3'
    Save $c $pal "$dir/${n}_cut.png"

    # polished: smooth face with an inset bevel
    $c = New-Canvas '4'; for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { if ((N $x $y ($seed + 1)) -gt 0.86) { Set-Px $c $x $y '5' } } }
    Bevel $c 0 0 15 15 '6' '2'; Bevel $c 2 2 13 13 '3' '5'
    Save $c $pal "$dir/${n}_polished.png"

    # slab side of the polished slab: two bevelled halves
    $c = New-Canvas '4'; Bevel $c 0 0 15 7 '6' '2'; Bevel $c 0 8 15 15 '6' '2'
    for ($x = 2; $x -le 13; $x++) { if ((N $x 3 $seed) -gt 0.7) { Set-Px $c $x 4 '5' }; if ((N $x 11 $seed) -gt 0.7) { Set-Px $c $x 12 '5' } }
    Save $c $pal "$dir/${n}_slab.png"

    # cut bricks: 4 courses of 8-wide bricks, staggered
    $c = New-Canvas; Noise-Fill $c 0 0 15 15 ($seed + 2) 0.18 0.85
    for ($row = 0; $row -lt 4; $row++) {
        $y0 = $row * 4; $off = if ($row % 2) { 4 } else { 0 }
        for ($b = -1; $b -lt 2; $b++) { $x0 = $b * 8 + $off; Bevel $c ([Math]::Max(0, $x0)) $y0 ([Math]::Min(15, $x0 + 7)) ($y0 + 3) '5' '2' }
        for ($x = 0; $x -lt 16; $x++) { Set-Px $c $x ($y0 + 3) '1' }
        Set-Px $c (($off + 7) % 16) $y0 '1'; Set-Px $c (($off + 7) % 16) ($y0 + 1) '1'; Set-Px $c (($off + 7) % 16) ($y0 + 2) '1'
        Set-Px $c (($off + 15) % 16) $y0 '1'; Set-Px $c (($off + 15) % 16) ($y0 + 1) '1'; Set-Px $c (($off + 15) % 16) ($y0 + 2) '1'
    }
    Save $c $pal "$dir/${n}_brick.png"

    # small bricks: 4x4 grid of small stones, staggered by 2
    $c = New-Canvas; Noise-Fill $c 0 0 15 15 ($seed + 3) 0.2 0.85
    for ($row = 0; $row -lt 4; $row++) {
        $y0 = $row * 4; $off = if ($row % 2) { 2 } else { 0 }
        for ($x = 0; $x -lt 16; $x++) { Set-Px $c $x ($y0 + 3) '1'; Set-Px $c $x $y0 '5' }
        for ($b = 0; $b -lt 4; $b++) { $mx = ($b * 4 + $off + 3) % 16; for ($y = $y0; $y -le $y0 + 2; $y++) { Set-Px $c $mx $y '1' }; Set-Px $c (($mx + 1) % 16) ($y0 + 1) '5' }
    }
    Save $c $pal "$dir/${n}_small_brick.png"

    # layered: horizontal strata of varying thickness
    $c = New-Canvas; $y = 0; $band = 0
    while ($y -lt 16) {
        $h = 2 + [int]((N $band 1 $seed) * 3); $k = @('3', '4', '5', '4')[$band % 4]
        for ($yy = $y; $yy -lt [Math]::Min(16, $y + $h); $yy++) { for ($x = 0; $x -lt 16; $x++) { Set-Px $c $x $yy ($(if ((N $x $yy $seed) -gt 0.9) { '6' } elseif ((N $x $yy ($seed + 5)) -lt 0.08) { '2' } else { $k })) } }
        if ($y + $h -lt 16) { for ($x = 0; $x -lt 16; $x++) { Set-Px $c $x ($y + $h - 1) '2' } }
        $y += $h; $band++
    }
    Save $c $pal "$dir/${n}_layered.png"

    # cap (top of layered / pillar): concentric bevelled squares
    $c = New-Canvas; Noise-Fill $c 0 0 15 15 ($seed + 4) 0.15 0.88
    Bevel $c 0 0 15 15 '5' '1'; Bevel $c 3 3 12 12 '2' '5'; Bevel $c 6 6 9 9 '5' '2'
    Save $c $pal "$dir/${n}_cap.png"

    # pillar side: vertical flutes with top and bottom rings
    $c = New-Canvas '4'
    for ($x = 0; $x -lt 16; $x++) { $k = @('5', '4', '3', '2')[$x % 4]; for ($y = 0; $y -lt 16; $y++) { Set-Px $c $x $y $k } }
    Fill $c 0 0 15 1 '5'; Fill $c 0 14 15 15 '3'; for ($x = 0; $x -lt 16; $x++) { Set-Px $c $x 1 '2'; Set-Px $c $x 14 '6' }
    Save $c $pal "$dir/${n}_pillar.png"

    # raw stone of Create's stones (4 natural variants for the ore-bearing ones)
    $natural = @{ asurine = 4; crimsite = 4; ochrum = 4; veridium = 4; limestone = 1; scoria = 1; scorchia = 1 }
    if ($natural.ContainsKey($n)) {
        for ($v = 0; $v -lt $natural[$n]; $v++) {
            $c = New-Canvas; $s2 = $seed + 31 * ($v + 1)
            for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
                $a = N $x $y $s2; $b = N ([int]($x / 2)) ([int]($y / 2)) ($s2 + 7)
                Set-Px $c $x $y ($(if ($b -lt 0.18) { '2' } elseif ($a -lt 0.25) { '3' } elseif ($a -gt 0.9) { '6' } elseif ($b -gt 0.8) { '5' } else { '4' }))
            } }
            # mineral streak (the ore the stone yields when crushed)
            $sy = 3 + [int]((N $v 0 $s2) * 9)
            for ($x = 0; $x -lt 16; $x++) { $yy = $sy + [int][Math]::Round([Math]::Sin(($x + $v * 3) / 2.5) * 1.5); Set-Px $c $x $yy '5'; if ((N $x $yy $s2) -gt 0.5) { Set-Px $c $x ($yy + 1) '6' } }
            $file = if ($natural[$n] -gt 1) { "${n}_natural_$v" } else { $n }
            Save $c $pal "$dir/$file.png"
        }
    }
}

# ---- metal / mineral blocks ---------------------------------------------------------------------
$metals = (Pal 'zinc' '#a7b8b0' 'brass' '#d9a441' 'copper' '#d47a4a' 'iron' '#c9c9c9' 'gold' '#f2cf47' 'andesite' '#9a9f93' 'rose' '#d96a77' 'obsidian' '#2f2440' 'radiant' '#f3f0e6' 'shadow' '#3b3150' 'chroma' '#7f5fb0' 'cardboard' '#b58b5a' 'exp' '#9fe35a' 'chocolate' '#6b3f22')
function Metal-Block($name, $base, $seed) {
    $pal = Ramp $base; $c = New-Canvas '4'
    for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { if ((N $x $y $seed) -gt 0.88) { Set-Px $c $x $y '5' } elseif ((N $x $y ($seed + 1)) -lt 0.06) { Set-Px $c $x $y '3' } } }
    Bevel $c 0 0 15 15 '6' '2'; Bevel $c 1 1 14 14 '5' '3'
    foreach ($p in @(@(3, 3), @(12, 3), @(3, 12), @(12, 12))) { Set-Px $c $p[0] $p[1] '2'; Set-Px $c ($p[0] + 1) ($p[1] + 1) '6' }
    Save $c $pal "$blocks/$name.png"
}
Metal-Block 'zinc_block' $metals.zinc 11
Metal-Block 'brass_block' $metals.brass 12
# andesite alloy block: plates with rivets
$c = New-Canvas; Noise-Fill $c 0 0 15 15 13; Bevel $c 0 0 15 15 '5' '1'; Bevel $c 0 0 7 7 '5' '2'; Bevel $c 8 8 15 15 '5' '2'
foreach ($p in @(@(2, 2), @(13, 2), @(2, 13), @(13, 13))) { Set-Px $c $p[0] $p[1] '6' }
Save $c (Ramp $metals.andesite) "$blocks/andesite_alloy_block.png"
# raw zinc block: lumpy
$c = New-Canvas; for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { $a = N ([int]($x / 3)) ([int]($y / 3)) 21; $b = N $x $y 22; Set-Px $c $x $y ($(if ($a -lt 0.3) { '2' } elseif ($b -gt 0.85) { '6' } elseif ($a -gt 0.7) { '5' } else { '4' })) } }
Save $c (Ramp '#8fa39a') "$blocks/raw_zinc_block.png"
# zinc ores: stone / deepslate with zinc clusters
function Ore($name, $stoneBase, $seed) {
    $pal = Merge-Pal (Ramp $stoneBase) (Pal 'z' '#c9dcd2' 'Z' '#7f998c' 'w' '#eef7f2')
    $c = New-Canvas; Noise-Fill $c 0 0 15 15 $seed 0.25 0.8
    foreach ($cl in @(@(3, 3), @(10, 2), @(6, 8), @(12, 10), @(2, 12))) {
        Set-Px $c $cl[0] $cl[1] 'z'; Set-Px $c ($cl[0] + 1) $cl[1] 'w'; Set-Px $c $cl[0] ($cl[1] + 1) 'Z'; Set-Px $c ($cl[0] + 1) ($cl[1] + 1) 'z'
        if ((N $cl[0] $cl[1] $seed) -gt 0.4) { Set-Px $c ($cl[0] + 2) ($cl[1] + 1) 'Z' }
    }
    Save $c $pal "$blocks/$name.png"
}
Ore 'zinc_ore' '#7d7d7d' 31
Ore 'deepslate_zinc_ore' '#4f4f55' 32
# industrial / weathered iron: dark plates with seams
foreach ($w in @(@('industrial_iron_block', '#62656a', 41), @('weathered_iron_block', '#6b5b4f', 42))) {
    $pal = Merge-Pal (Ramp $w[1]) (Pal 'r' '#8a5a3a')
    $c = New-Canvas; Noise-Fill $c 0 0 15 15 $w[2] 0.15 0.9; Bevel $c 0 0 15 15 '5' '1'
    for ($y = 0; $y -lt 16; $y++) { Set-Px $c 7 $y '2'; Set-Px $c 8 $y '5' }
    if ($w[0] -like 'weathered*') { for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { if ((N $x $y 77) -gt 0.86) { Set-Px $c $x $y 'r' } } } }
    Save $c $pal "$blocks/$($w[0]).png"
    $c = New-Canvas; Noise-Fill $c 0 0 15 15 ($w[2] + 5) 0.15 0.9; Bevel $c 0 0 15 15 '5' '1'; Bevel $c 3 3 12 12 '2' '5'
    if ($w[0] -like 'weathered*') { for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { if ((N $x $y 78) -gt 0.86) { Set-Px $c $x $y 'r' } } } }
    Save $c $pal "$blocks/$($w[0])_top.png"
}
# rose quartz block (pillar), tiles, small tiles
$rq = Ramp $metals.rose
$c = New-Canvas; for ($x = 0; $x -lt 16; $x++) { for ($y = 0; $y -lt 16; $y++) { Set-Px $c $x $y (@('4', '5', '4', '3')[(($x + [int]($y / 4)) % 4)]) } }
Bevel $c 0 0 15 15 '6' '2'; Save $c $rq "$blocks/rose_quartz_block.png"
$c = New-Canvas '4'; Bevel $c 0 0 15 15 '6' '2'; Bevel $c 4 4 11 11 '2' '6'; Fill $c 6 6 9 9 '5'; Save $c $rq "$blocks/rose_quartz_block_top.png"
$c = New-Canvas '4'; foreach ($t in @(@(0, 0), @(8, 0), @(0, 8), @(8, 8))) { Bevel $c $t[0] $t[1] ($t[0] + 7) ($t[1] + 7) '6' '2'; Set-Px $c ($t[0] + 2) ($t[1] + 2) '5' }
Save $c $rq "$blocks/rose_quartz_tiles.png"
$c = New-Canvas '4'; for ($ty = 0; $ty -lt 4; $ty++) { for ($tx = 0; $tx -lt 4; $tx++) { Bevel $c ($tx * 4) ($ty * 4) ($tx * 4 + 3) ($ty * 4 + 3) '6' '2' } }
Save $c $rq "$blocks/small_rose_quartz_tiles.png"
# experience block: glowing green with sparkles
$c = New-Canvas; for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { $d = [Math]::Abs($x - 7.5) + [Math]::Abs($y - 7.5); Set-Px $c $x $y ($(if ($d -lt 4) { '6' } elseif ($d -lt 8) { '5' } elseif ($d -lt 12) { '4' } else { '3' })) } }
foreach ($p in @(@(3, 4), @(11, 3), @(4, 11), @(12, 12), @(7, 1))) { Set-Px $c $p[0] $p[1] 'w' }
Bevel $c 0 0 15 15 '5' '2'; Save $c (Merge-Pal (Ramp $metals.exp) (Pal 'w' '#ffffe0')) "$blocks/experience_block.png"
# cardboard blocks: corrugated sides, layered end; bound one gets string
$cb = Merge-Pal (Ramp $metals.cardboard) (Pal 's' '#ece6d6' 'S' '#b9b2a2')
foreach ($bound in $false, $true) {
    $name = if ($bound) { 'bound_cardboard_block' } else { 'cardboard_block' }
    $c = New-Canvas; for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { Set-Px $c $x $y (@('4', '5', '4', '3')[$y % 4]) } }
    Bevel $c 0 0 15 15 '5' '2'
    if ($bound) { for ($y = 0; $y -lt 16; $y++) { Set-Px $c 7 $y 's'; Set-Px $c 8 $y 'S' }; for ($x = 0; $x -lt 16; $x++) { Set-Px $c $x 7 's'; Set-Px $c $x 8 'S' } }
    Save $c $cb "$blocks/$name.png"
    $c = New-Canvas; for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { Set-Px $c $x $y (@('4', '3', '5', '4', '2')[$x % 5]) } }
    Bevel $c 0 0 15 15 '5' '2'
    if ($bound) { for ($x = 0; $x -lt 16; $x++) { Set-Px $c $x 7 's'; Set-Px $c $x 8 'S' } }
    Save $c $cb "$blocks/${name}_end.png"
}

# ---- items --------------------------------------------------------------------------------------
# Templates: '.' transparent, 1..6 the item's ramp (1 = outline), extra keys per item.
$T = @{
    ingot = @(
        '................', '................', '................', '................',
        '................', '.......1111111..', '.....11566655441', '...115665555431.',
        '..1566555544431.', '.15555544443321.', '.1444444333221..', '.1333333322211..',
        '..11112222111...', '......1111......', '................', '................')
    nugget = @(
        '................', '................', '................', '................',
        '................', '......111.......', '....1166511.....', '...165554431....',
        '...155444321....', '...14443332 1...'.Replace(' ', '1'), '....13322211....', '.....11111......',
        '................', '................', '................', '................')
    sheet = @(
        '................', '................', '................', '..111111111111..',
        '..166666666651..', '..165555555541..', '..165555555541..', '..155555555441..',
        '..155555554441..', '..154555544441..', '..154445444431..', '..144444444331..',
        '..143333333321..', '..111111111111..', '................', '................')
    raw = @(
        '................', '................', '.....1111.......', '....156641......',
        '...15554431111..', '..1555443315661.', '..144433315554 1'.Replace(' ', '1'), '..13433215444321',
        '..1332221443321.', '...1221144332 1.'.Replace(' ', '1'), '....11.1322211..', '........1111....',
        '................', '................', '................', '................')
    crushed = @(
        '................', '................', '.......11.......', '......1641..11..',
        '..11..1551.1661.', '.1661..11..1541.', '.1541.11....11..', '..11.1651.11....',
        '.....1541.1651..', '..11..11..1431..', '.1561.111..11...', '.1431.1541..11..',
        '..11..1331.1561.', '.......11..1431.', '............11..', '................')
    powder = @(
        '................', '................', '................', '................',
        '................', '................', '.......11.......', '......1651......',
        '.....155541.....', '....15545441....', '...1554444431...', '..155444343331..',
        '..144433333221..', '...1111111111...', '................', '................')
    blob = @(
        '................', '................', '................', '................',
        '.....111111.....', '...1165556611...', '..165555555541..', '.15555555544441.',
        '.15555554444431.', '.14444444443331.', '..133333333221..', '...1122222211...',
        '.....111111.....', '................', '................', '................')
    gem = @(
        '................', '................', '.......11.......', '......1661......',
        '.....156651.....', '....15665541....', '...1566555441...', '...1555554431...',
        '...1455544331...', '....14443331....', '.....143321.....', '......1321......',
        '.......11.......', '................', '................', '................')
    cluster = @(
        '................', '................', '.....1....1.....', '....161..161....',
        '....151.1651....', '...1551.15541...', '...1541.14431.1.', '..15541114431161',
        '..14431156331551', '..14331155421441', '...1321144321331', '....111133211221',
        '.......111111111', '................', '................', '................')
}
function Item-Art([string]$name, [string]$tpl, $base, $extra = @{}) {
    $pal = Merge-Pal (Ramp $base) $extra
    $pal['1'] = Shade-Color $base 0.35
    Save-Rows $T[$tpl] $pal "$items/$name.png"
}
Item-Art 'zinc_ingot' ingot $metals.zinc; Item-Art 'brass_ingot' ingot $metals.brass; Item-Art 'andesite_alloy' ingot $metals.andesite
Item-Art 'zinc_nugget' nugget $metals.zinc; Item-Art 'brass_nugget' nugget $metals.brass; Item-Art 'copper_nugget' nugget $metals.copper
Item-Art 'experience_nugget' nugget $metals.exp
Item-Art 'zinc_sheet_unused' sheet $metals.zinc; Remove-Item "$items/zinc_sheet_unused.png"
Item-Art 'brass_sheet' sheet $metals.brass; Item-Art 'copper_sheet' sheet $metals.copper; Item-Art 'iron_sheet' sheet $metals.iron; Item-Art 'golden_sheet' sheet $metals.gold
Item-Art 'sturdy_sheet' sheet '#4a3d63'; Item-Art 'cardboard' sheet $metals.cardboard
Item-Art 'raw_zinc' raw '#9db3a8'
foreach ($m in @(@('iron', '#c9b39b'), @('gold', '#f2cf47'), @('copper', '#d47a4a'), @('zinc', '#a7c4b6'), @('osmium', '#9fb3c8'), @('platinum', '#bfe3ee'),
        @('silver', '#dfe4ea'), @('tin', '#c8cdd0'), @('lead', '#6a6f8a'), @('quicksilver', '#e0e8f0'), @('aluminum', '#d9dcde'), @('uranium', '#8fd16a'), @('nickel', '#d8d3b0'))) {
    Item-Art "crushed_raw_$($m[0])" crushed $m[1]
}
Item-Art 'powdered_obsidian' powder '#3d2f55'; Item-Art 'cinder_flour' powder '#8a3a33'; Item-Art 'wheat_flour' powder '#e8e1c8'
Item-Art 'dough' blob '#e3d6b0'; Item-Art 'pulp' blob '#b9ab8a'; Item-Art 'blaze_cake_base' blob '#d6a463'
Item-Art 'rose_quartz' cluster $metals.rose; Item-Art 'polished_rose_quartz' gem $metals.rose
Item-Art 'refined_radiance' ingot $metals.radiant; Item-Art 'shadow_steel' ingot $metals.shadow; Item-Art 'chromatic_compound' gem $metals.chroma

# hand-made sprites
function Sprite([string]$name, [string[]]$rows, $pal) { Save-Rows $rows $pal "$items/$name.png" }
$brassPal = Merge-Pal (Ramp $metals.brass) (Pal 'g' '#bfe8ff' 'G' '#7fb6d6' 'w' '#ffffff' 'r' '#d96a77' 'R' '#9a3a48' 'i' '#8d8d8d' 'I' '#5d5d5d')
Sprite 'electron_tube' @(
    '................', '......1111......', '.....1gggg1.....', '....1gwGGgg1....',
    '....1gwGrGg1....', '....1ggRrGg1....', '....1gGrRGg1....', '....1ggGGgg1....',
    '.....1gggg1.....', '....11iiii11....', '....1iIiiIi1....', '....1IiIIiI1....',
    '.....1i11i1.....', '.....1I..I1.....', '......1..1......', '................') $brassPal
Sprite 'precision_mechanism' @(
    '................', '.....11..11.....', '....1551155 1...'.Replace(' ', '1'), '..115445544511..',
    '..154431134451..', '...1431661341...', '.1154316613451..', '.15443166134451.',
    '.14433111134331.', '..113431134311..', '...1433113341...', '..153321123351..',
    '..11221122112 1.'.Replace(' ', '1'), '....11.11.11....', '................', '................') (Merge-Pal (Ramp $metals.gold) (Pal '1' '#5a4512'))
Sprite 'incomplete_precision_mechanism' @(
    '................', '................', '................', '..111111111111..',
    '..166666666651..', '..165511155541..', '..1551.1.15541..', '..1551...15441..',
    '..1551.1.15441..', '..154511154441..', '..154445444431..', '..144444444331..',
    '..143333333321..', '..111111111111..', '................', '................') (Merge-Pal (Ramp $metals.gold) (Pal '1' '#5a4512'))
$ironPal = Merge-Pal (Ramp $metals.iron) (Pal '1' '#3c3c3c' 'a' '#9a9f93' 'A' '#6c7068')
Sprite 'propeller' @(
    '................', '......111.......', '.....15541......', '.....15431......',
    '.....15431......', '......1431.111..', '..111.1aa1155411', '.1554411aA1444331',
    '.13344411aa1111.'.Substring(0, 16), '..111.1Aa1.....'.PadRight(16, '.'), '......1341......', '.....13431......',
    '.....13321......', '.....13321......', '......111.......', '................') $ironPal
Sprite 'whisk' @(
    '................', '.......11.......', '......1aa1......', '......1Aa1......',
    '......1aA1......', '.....11aa11.....', '....15.11.51....', '...15.1551.51...',
    '...14.1441.41...', '...14.1441.41...', '...13.1331.31...', '...13.1331.31...',
    '....13.11.31....', '.....1333331....', '......11111.....', '................') $ironPal
Sprite 'brass_hand' @(
    '................', '....1.1.1.......', '...151515 1.....'.Replace(' ', '1'), '...14141515.....',
    '...1414141 41...'.Replace(' ', '1'), '...14141414 1...'.Replace(' ', '1'), '...144444441....', '...14444444 1...'.Replace(' ', '1'),
    '....1444443 1...'.Replace(' ', '1'), '....1333331.....', '.....1aaa1......', '.....1aAa1......',
    '.....1aaa1......', '.....1AAA1......', '......111.......', '................') (Merge-Pal $brassPal (Pal 'a' '#9a9f93' 'A' '#6c7068'))
Sprite 'crafter_slot_cover' @(
    '................', '................', '................', '................',
    '................', '..111111111111..', '..155555555551..', '..154444444431..',
    '..154111111431..', '..154444444431..', '..133333333331..', '..111111111111..',
    '................', '................', '................', '................') $brassPal
Sprite 'unprocessed_obsidian_sheet' @(
    '................', '................', '................', '................',
    '...1111111111...', '..155454545541..', '..145444544431..', '..154544454341..',
    '..144454443331..', '..143444343321..', '..133332333221..', '...1111111111...',
    '................', '................', '................', '................') (Merge-Pal (Ramp '#3d2f55') (Pal '1' '#160f22'))
Sprite 'transmitter' @(
    '................', '.......11.......', '......1661......', '......1551......',
    '......1441......', '.....111111.....', '....1cCcCcC1....', '...1CcCcCcCc1...',
    '...1cCcCcCcC1...', '....1CcCcCc1....', '.....111111.....', '......1rr1......',
    '......1Rr1......', '......1rR1......', '.......11.......', '................') (Merge-Pal (Ramp $metals.copper) (Pal '1' '#3d2416' 'c' '#e6955e' 'C' '#a5552b' 'r' '#e03a2a' 'R' '#9a1f16'))
$chocPal = Merge-Pal (Ramp $metals.chocolate) (Pal '1' '#2a170b')
Sprite 'bar_of_chocolate' @(
    '................', '................', '................', '..111111111111..',
    '..156516551651..', '..154314431431..', '..133113311331..', '..156516551651..',
    '..154314431431..', '..133113311331..', '..156516551651..', '..154314431431..',
    '..133113311331..', '..111111111111..', '................', '................') $chocPal
Sprite 'sweet_roll' @(
    '................', '................', '................', '.....111111.....',
    '...11wwwwww11...', '..1wwWwwwwWww1..', '.1wWwwccccwwW41.', '.1wwcc4444ccw41.',
    '.14c445555445c1.', '.1445566665544 1'.Replace(' ', '1'), '.1445566655443 1'.Substring(0, 16).Replace(' ', '1'), '..1344455443321.',
    '...11333333211..', '.....111111.....', '................', '................') (Merge-Pal (Ramp '#c98a46') (Pal '1' '#4a2c12' 'w' '#f6f1e6' 'W' '#ddd6c6' 'c' '#a8692f'))
Sprite 'chocolate_glazed_berries' @(
    '................', '.........11.....', '........1gg1....', '.......1gG1.....',
    '......11g1......', '....11cC1C11....', '...1cCcc1cCc1...', '...1cccC1ccC1...',
    '..11cCc111cc11..', '.1cCcc1r1cCcCc1.', '.1ccCC1R1cccC 1.'.Replace(' ', '1'), '.1cCcc111cCcc1..',
    '..1cc1...1cc1...', '...11.....11....', '................', '................') ((Pal '1' '#2a170b' 'c' '#6b3f22' 'C' '#8f5a33' 'r' '#c0263a' 'R' '#7a1220' 'g' '#4f8f3a' 'G' '#2f6024'))
Sprite 'honeyed_apple' @(
    '................', '.......11.......', '.......1b1......', '......11b1......',
    '....111 111.....'.Replace(' ', '1'), '...1hhhrrrrr1...', '..1hHhrrrrrrr1..', '..1hhrrrrrrrr1..',
    '..1hHrrrrRRrr1..', '..1hhrrrrRRrr1..', '..1rhHrrrRrrr1..', '...1hhrrrrrr1...',
    '...1hHhrrrrr1...', '....1hh111h1....', '.....11..1h1....', '..........1.....') ((Pal '1' '#3a1408' 'r' '#c8392b' 'R' '#8e2117' 'h' '#f2b33a' 'H' '#ffd76a' 'b' '#5b3a1e'))
Sprite 'builders_tea' @(
    '................', '................', '...1111111111...', '...1gggggggg1...',
    '...1tttttttt111.', '...1tTttttTt1.1.', '...1ttTtttTt1.1.', '...1tttTtttt1.1.',
    '...1tTtttTtt111.', '...1ttttTttt1...', '...1tTttttTt1...', '...1tttTtttt1...',
    '....1tttttt1....', '.....111111.....', '................', '................') ((Pal '1' '#2e3a2a' 'g' '#d8f0f8' 't' '#7a8f3a' 'T' '#a0b85a'))
$cakePal = (Pal '1' '#3a1a08' 'c' '#d6a463' 'C' '#a8723a' 'f' '#ff9f1a' 'F' '#ffd34a' 'e' '#e25a12')
Sprite 'blaze_cake' @(
    '................', '................', '.....f..F..f....', '....fF.fFf.Ff...',
    '...1eFfeFFefF1..', '..1eeeeeeeeeee1.', '..1cCcCcCcCcCc1.', '..1cccccccccccc1'.Substring(0, 16), '..1CcCcCcCcCcC1.',
    '..1ccccccccccc1.', '..1cCcCcCcCcCc1.', '..1CCCCCCCCCCC1.', '...11111111111..', '................',
    '................', '................') $cakePal
Sprite 'creative_blaze_cake' @(
    '................', '................', '.....p..P..p....', '....pP.pPp.Pp...',
    '...1qPpqPPqpP1..', '..1qqqqqqqqqqq1.', '..1cCcCcCcCcCc1.', '..1cccccccccccc1'.Substring(0, 16), '..1CcCcCcCcCcC1.',
    '..1ccccccccccc1.', '..1cCcCcCcCcCc1.', '..1CCCCCCCCCCC1.', '...11111111111..', '................',
    '................', '................') (Merge-Pal $cakePal (Pal 'p' '#d65aff' 'P' '#f2b0ff' 'q' '#8a2be2'))
