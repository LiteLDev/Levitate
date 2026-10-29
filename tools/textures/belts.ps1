# Textures of the belts unit (original pixel art): mechanical belt (plain + 16 dyed), its pulley and
# casings, depot, weighted ejector and the belt connector item.
#   pwsh tools/textures/belts.ps1
. "$PSScriptRoot/lib.ps1"
$blocks = Join-Path $PSScriptRoot '../../assets/pack/rp/textures/blocks'
$items  = Join-Path $PSScriptRoot '../../assets/pack/rp/textures/items'

# ---- belt band (16x16, ribs across the width so the scrolling shows) -----------------------------
$band = @(
    'eddddddddddddde',
    'edbbbbbbbbbbbde',
    'edbbbbbbbbbbbde',
    'edccccccccccdde',
    'edbbbbbbbbbbbde',
    'edbbbbbbbbbbbde',
    'edbbbbbbbbbbbde',
    'eddddddddddddde',
    'eddddddddddddde',
    'edbbbbbbbbbbbde',
    'edbbbbbbbbbbbde',
    'edccccccccccdde',
    'edbbbbbbbbbbbde',
    'edbbbbbbbbbbbde',
    'edbbbbbbbbbbbde',
    'eddddddddddddde'
) | ForEach-Object { $_ + $_.Substring(14, 1) }   # 16 wide

$plain = @{ e = '#1d1b1a'; d = '#2d2a28'; b = '#45403c'; c = '#5d5751' }
Save-PixelArt -Rows $band -Palette $plain -Path "$blocks/belt.png"

# Java dye order (white .. black), the order of the levitate:color state
$dyes = [ordered]@{
    white = '#d8dcdc'; orange = '#e0712a'; magenta = '#b84ab0'; light_blue = '#4aa8d8'
    yellow = '#e8c230'; lime = '#78c22a'; pink = '#e48ca8'; gray = '#4a4f52'
    light_gray = '#9a9c96'; cyan = '#178c90'; purple = '#7a32aa'; blue = '#38429c'
    brown = '#6e4a2c'; green = '#566e22'; red = '#a8302a'; black = '#1c1c22'
}
foreach ($name in $dyes.Keys) {
    $base = $dyes[$name]
    $pal  = @{ e = (Shade-Color $base 0.45); d = (Shade-Color $base 0.62); b = (Shade-Color $base 0.85); c = $base }
    Save-PixelArt -Rows $band -Palette $pal -Path "$blocks/belt_$name.png"
}

# ---- pulley (shaft through a hub; 16x16: shaft side x 0..3, hub side x 4..15, hub face y 12..15) ----
$pulley = @(
    'bcabhhhhhhhhhhhh',
    'bcabhkkkkkkkkkkh',
    'bcabhkhhhhhhhhkh',
    'bcabhkhkkkkkkhkh',
    'bcabhkhkkkkkkhkh',
    'bcabhkhhhhhhhhkh',
    'bcabhkkkkkkkkkkh',
    'bcabhhhhhhhhhhhh',
    'bcabnnnnnnnnnnnn',
    'bcabhkkkkkkkkkkh',
    'bcabhkkkkkkkkkkh',
    'beebnnnnnnnnnnnn',
    'eooeoooooooooooo',
    'eooeohhhhhhhhhho',
    'eooeohkkkkkkkkho',
    'beeboooooooooooo'
)
Save-PixelArt -Rows $pulley -Palette @{ b = '#6b706d'; c = '#a9aeab'; a = '#8a8f8c'; e = '#4a4e4c'; o = '#2f2f2f'; h = '#8c6b45'; k = '#a8865a'; n = '#5e4529' } -Path "$blocks/belt_pulley.png"

# ---- belt casings (andesite / brass frame) --------------------------------------------------------
$casing = @(
    'oooooooooooooooo',
    'ollllllllllllllo',
    'olmmmmmmmmmmmmlo',
    'olmddddddddddmlo',
    'olmdmmmmmmmmdmlo',
    'olmdmmmmmmmmdmlo',
    'olmdmmmmmmmmdmlo',
    'olmdmmmmmmmmdmlo',
    'olmdmmmmmmmmdmlo',
    'olmdmmmmmmmmdmlo',
    'olmdmmmmmmmmdmlo',
    'olmdmmmmmmmmdmlo',
    'olmddddddddddmlo',
    'olmmmmmmmmmmmmlo',
    'ollllllllllllllo',
    'oooooooooooooooo'
)
Save-PixelArt -Rows $casing -Palette @{ o = '#4e524f'; l = '#a7aba5'; m = '#8b8f89'; d = '#6c706b' } -Path "$blocks/belt_casing_andesite.png"
Save-PixelArt -Rows $casing -Palette @{ o = '#5a3d1f'; l = '#e8b765'; m = '#c9933f'; d = '#9c6d2b' } -Path "$blocks/belt_casing_brass.png"

# ---- depot ------------------------------------------------------------------------------------------
$depotTop = @(
    'oooooooooooooooo',
    'olllllllllllllll',
    'olkkkkkkkkkkkkml',
    'olkddddddddddkml',
    'olkdssssssssdkml',
    'olkdssssssssdkml',
    'olkdssssssssdkml',
    'olkdssssssssdkml',
    'olkdssssssssdkml',
    'olkdssssssssdkml',
    'olkdssssssssdkml',
    'olkdssssssssdkml',
    'olkddddddddddkml',
    'olkkkkkkkkkkkkml',
    'olmmmmmmmmmmmmml',
    'olllllllllllllll'
)
$depotSide = @(
    'llllllllllllllll',
    'mkkkkkkkkkkkkkkm',
    'mkkkkkkkkkkkkkkm',
    'oooooooooooooooo',
    'olllllllllllllll',
    'olmmmmmmmmmmmmml',
    'olmddddmmddddmml',
    'olmdmmdmmdmmdmml',
    'olmdmmdmmdmmdmml',
    'olmddddmmddddmml',
    'olmmmmmmmmmmmmml',
    'olmmmmmmmmmmmmml',
    'olmmmmmmmmmmmmml',
    'olllllllllllllll',
    'oooooooooooooooo',
    'oooooooooooooooo'
)
$depotBottom = $casing
$andesite = @{ o = '#4e524f'; l = '#a7aba5'; m = '#8b8f89'; d = '#6c706b'; k = '#7a7e78'; s = '#5b5f5a' }
Save-PixelArt -Rows $depotTop -Palette $andesite -Path "$blocks/depot_top.png"
Save-PixelArt -Rows $depotSide -Palette $andesite -Path "$blocks/depot_side.png"
Save-PixelArt -Rows $depotBottom -Palette $andesite -Path "$blocks/depot_bottom.png"

# ---- weighted ejector (andesite base, golden launch plate, shaft) -----------------------------------
$plate = @(
    'gggggggggggggggg',
    'gyyyyyyyyyyyyyyg',
    'gyhhhhhhhhhhhhyg',
    'gyhyyyyyyyyyyhyg',
    'gyhyhhhhhhhhyhyg',
    'gyhyhyyyyyyhyhyg',
    'gyhyhyggggyhyhyg',
    'gyhyhyggggyhyhyg',
    'gyhyhyggggyhyhyg',
    'gyhyhyggggyhyhyg',
    'gyhyhyyyyyyhyhyg',
    'gyhyhhhhhhhhyhyg',
    'gyhyyyyyyyyyyhyg',
    'gyhhhhhhhhhhhhyg',
    'gyyyyyyyyyyyyyyg',
    'gggggggggggggggg'
)
Save-PixelArt -Rows $plate -Palette @{ g = '#8a6a1c'; y = '#e8c24a'; h = '#c49a2c' } -Path "$blocks/weighted_ejector_top.png"
$ejSide = @(
    'gggggggggggggggg',
    'yyyyyyyyyyyyyyyy',
    'oooooooooooooooo',
    'olllllllllllllll',
    'olmmmmmmmmmmmmml',
    'olmdddmmmmdddmml',
    'olmdmmmmmmmmdmml',
    'olmdmmmeemmmdmml',
    'olmdmmmeemmmdmml',
    'olmdmmmmmmmmdmml',
    'olmdddmmmmdddmml',
    'olmmmmmmmmmmmmml',
    'olmmmmmmmmmmmmml',
    'olllllllllllllll',
    'oooooooooooooooo',
    'oooooooooooooooo'
)
Save-PixelArt -Rows $ejSide -Palette @{ g = '#8a6a1c'; y = '#e8c24a'; o = '#4e524f'; l = '#a7aba5'; m = '#8b8f89'; d = '#6c706b'; e = '#2f2f2f' } -Path "$blocks/weighted_ejector_side.png"
Save-PixelArt -Rows $casing -Palette $andesite -Path "$blocks/weighted_ejector_bottom.png"
# the rotating shaft (x 0..3 side, x 4..7 y 0..3 end), same layout as the kinetic test shaft
$shaft = @()
for ($y = 0; $y -lt 16; $y++) {
    $side = @('bcab', 'bcab', 'bcab', 'beeb')[$y % 4]
    $end  = if ($y -lt 4) { @('beeb', 'eooe', 'eooe', 'beeb')[$y] } else { '....' }
    $shaft += "$side$end........"
}
Save-PixelArt -Rows $shaft -Palette @{ b = '#6b706d'; c = '#a9aeab'; a = '#8a8f8c'; e = '#4a4e4c'; o = '#2f2f2f' } -Path "$blocks/weighted_ejector_shaft.png"

# ---- belt connector item (a rolled up belt) ------------------------------------------------------------
$connector = @(
    '................',
    '.....dddddd.....',
    '...ddbbbbbbdd...',
    '..dbbccccccbbd..',
    '..dbcddddddcbd..',
    '.dbcdbbbbbbdcbd.',
    '.dbcdboooobdcbd.',
    '.dbcdboooobdcbd.',
    '.dbcdboooobdcbd.',
    '.dbcdboooobdcbd.',
    '.dbcdbbbbbbdcbd.',
    '..dbcddddddcbd..',
    '..dbbccccccbbbd.',
    '...ddbbbbbbddbd.',
    '.....dddddd..dd.',
    '................'
)
Save-PixelArt -Rows $connector -Palette @{ d = '#1d1b1a'; b = '#45403c'; c = '#5d5751'; o = '#8c6b45' } -Path "$items/belt_connector.png"
Write-Host 'belts textures written'
