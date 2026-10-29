# Textures, block models and block definitions of the item logistics unit (U12): funnels,
# tunnels, chutes, smart chutes, item vaults (connected textures), creative crate.
# Original pixel art and models.
#
#   pwsh tools/textures/funnels.ps1            # writes RP textures/models, BP blocks, RP terrain_texture.d / blocks.d fragments
#   pwsh tools/textures/funnels.ps1 -Preview   # also writes 8x previews to tools/textures/preview/
param([switch]$Preview)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/lib.ps1"
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$tex = Join-Path $root 'assets/pack/rp/textures/blocks'
$models = Join-Path $root 'assets/pack/rp/models/blocks'
$bpBlocks = Join-Path $root 'assets/pack/bp/blocks'
$terrain = [ordered]@{}

# --- canvas helpers (16x16 char grids) -------------------------------------------------------------
function New-Canvas([char]$fill = '.') { , (0..15 | ForEach-Object { , ([char[]]((New-Object string $fill, 16))) }) }
function Set-Px($c, $x, $y, [char]$k) { if ($x -ge 0 -and $x -lt 16 -and $y -ge 0 -and $y -lt 16) { $c[$y][$x] = $k } }
function Fill($c, $x0, $y0, $x1, $y1, [char]$k) { for ($y = $y0; $y -le $y1; $y++) { for ($x = $x0; $x -le $x1; $x++) { Set-Px $c $x $y $k } } }
function Rows($c) { , ($c | ForEach-Object { -join $_ }) }
function Save($c, $pal, $name) {
    $rows = Rows $c
    Save-PixelArt -Rows $rows -Palette $pal -Path "$tex/$name.png"
    if ($Preview) { Save-PixelArt -Rows $rows -Palette $pal -Path (Join-Path $root "tools/textures/preview/$name.png") -Scale 8 }
    $terrain["levitate_$name"] = [ordered]@{ textures = "textures/blocks/$name" }
}
function Save-Json($path, $obj) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
    $text = ($obj | ConvertTo-Json -Depth 40) -replace "`r`n", "`n"
    [IO.File]::WriteAllText($path, $text + "`n", [Text.UTF8Encoding]::new($false))
}

# --- palettes ----------------------------------------------------------------------------------------
# metal shades: L light, M mid, D dark, K outline, R rivet; kelp curtain: g / h; dark inside: x / y
$andesite = @{ L = '#b3b8ad'; M = '#93988d'; D = '#6f736a'; K = '#4b4e47'; R = '#c9cdc2'; g = '#3d4a33'; h = '#56663f'; x = '#1d1f1c'; y = '#2c2f2a'; A = '#d9893b'; B = '#4a8fd4'; P = '#c23b3b' }
$brass = @{ L = '#f2d27a'; M = '#d9a941'; D = '#a8782a'; K = '#6e4b1a'; R = '#fff0b8'; g = '#3d4a33'; h = '#56663f'; x = '#1d1f1c'; y = '#2c2f2a'; A = '#d9893b'; B = '#4a8fd4'; P = '#c23b3b' }
$iron = @{ L = '#d4d7da'; M = '#a9adb2'; D = '#7d8187'; K = '#55585d'; R = '#eef0f2'; x = '#1d1f22'; y = '#34373b'; w = '#a8d8e8'; v = '#d9f1f8'; i = '#5b5752'; j = '#77726b'; P = '#c23b3b' }
$vaultPal = @{ L = '#c7cbd0'; M = '#a0a5ab'; D = '#7a8087'; K = '#4c5157'; R = '#e6e9ec'; G = '#d9b44a'; H = '#a8862e'; x = '#2a2d31' }
$creative = @{ L = '#d6a5e6'; M = '#a46cc0'; D = '#6e3f8a'; K = '#3f2152'; R = '#f2d6fb'; x = '#22132c'; y = '#352042' }

# --- shared drawings ----------------------------------------------------------------------------------
function Metal($c) {
    for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { Set-Px $c $x $y ($(if ((($x * 7 + $y * 5) % 11) -eq 0) { 'D' } else { 'M' })) } }
}
function Bevel($c, $x0 = 0, $y0 = 0, $x1 = 15, $y1 = 15) {
    Fill $c $x0 $y0 $x1 $y0 'L'; Fill $c $x0 $y0 $x0 $y1 'L'; Fill $c $x0 $y1 $x1 $y1 'K'; Fill $c $x1 $y0 $x1 $y1 'K'
}
function Rivets($c, $inset = 2) {
    foreach ($p in @(@($inset, $inset), @((15 - $inset), $inset), @($inset, (15 - $inset)), @((15 - $inset), (15 - $inset)))) { Set-Px $c $p[0] $p[1] 'R' }
}
# a frame around a dark opening
function Frame($c, $w = 3) {
    Metal $c; Bevel $c
    Fill $c $w $w (15 - $w) (15 - $w) 'x'
    Fill $c $w $w (15 - $w) $w 'y'; Fill $c $w $w $w (15 - $w) 'y'
    Fill $c ($w - 1) ($w - 1) (16 - $w) ($w - 1) 'K'; Fill $c ($w - 1) ($w - 1) ($w - 1) (16 - $w) 'K'
}
function Curtain($c) {
    for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { Set-Px $c $x $y ($(if (($x % 4) -eq 3) { 'x' } elseif ((($x + $y) % 5) -eq 0) { 'h' } else { 'g' })) } }
}

# --- funnels -------------------------------------------------------------------------------------------
foreach ($kind in @(@{ n = 'andesite'; p = $andesite }, @{ n = 'brass'; p = $brass })) {
    $p = $kind.p; $n = $kind.n
    $c = New-Canvas; Frame $c; Rivets $c 1; Save $c $p "${n}_funnel_frame"
    $c = New-Canvas; Metal $c; Bevel $c; Fill $c 2 7 13 8 'D'; Rivets $c 2; Save $c $p "${n}_funnel_side"
    $c = New-Canvas; Curtain $c; Save $c $p "${n}_funnel_flap"
    # the plate against the inventory: the stripe shows the direction (extract / insert / powered)
    foreach ($v in @(@{ s = 'extract'; k = 'A' }, @{ s = 'insert'; k = 'B' }, @{ s = 'powered'; k = 'P' })) {
        $c = New-Canvas; Metal $c; Bevel $c; Rivets $c 2
        Fill $c 4 4 11 11 'D'; Fill $c 5 5 10 10 $v.k
        if ($v.s -eq 'extract') { Fill $c 7 5 8 6 'R' } elseif ($v.s -eq 'insert') { Fill $c 7 9 8 10 'R' } else { Fill $c 6 7 9 8 'R' }
        Save $c $p "${n}_funnel_back_$($v.s)"
    }
}

# --- tunnels -------------------------------------------------------------------------------------------
foreach ($kind in @(@{ n = 'andesite'; p = $andesite }, @{ n = 'brass'; p = $brass })) {
    $p = $kind.p; $n = $kind.n
    $c = New-Canvas; Metal $c; Bevel $c; Fill $c 0 7 15 8 'D'; Fill $c 0 7 15 7 'K'; Rivets $c 2; Save $c $p "${n}_tunnel_wall"
    $c = New-Canvas; Metal $c; Bevel $c; Fill $c 3 3 12 12 'K'; Fill $c 4 4 11 11 'y'; Fill $c 5 5 10 10 'x'; Rivets $c 1; Save $c $p "${n}_tunnel_window"
    $c = New-Canvas; Metal $c; Bevel $c; Fill $c 2 2 13 13 'D'; Fill $c 3 3 12 12 'M'; Rivets $c 1
    if ($n -eq 'brass') { Fill $c 6 6 9 9 'K'; Fill $c 7 7 8 8 'R' }
    Save $c $p "${n}_tunnel_top"
    $c = New-Canvas; Metal $c; Fill $c 0 0 15 15 'D'; Fill $c 1 0 14 15 'M'; Fill $c 0 0 15 0 'L'; Save $c $p "${n}_tunnel_pillar"
    $c = New-Canvas; Curtain $c; Save $c $p "${n}_tunnel_flap"
}

# --- chutes --------------------------------------------------------------------------------------------
$c = New-Canvas; Metal $c; Bevel $c; Fill $c 0 3 15 3 'K'; Fill $c 0 12 15 12 'K'; Fill $c 0 4 15 4 'L'; Fill $c 0 13 15 13 'L'; Save $c $iron 'chute_side'
$c = New-Canvas; Metal $c; Bevel $c; Fill $c 0 3 15 3 'K'; Fill $c 0 12 15 12 'K'; Fill $c 2 5 13 10 'w'; Fill $c 3 6 6 7 'v'; Save $c $iron 'chute_window'
$c = New-Canvas; Frame $c 2; Save $c $iron 'chute_top'
$c = New-Canvas
for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) { Set-Px $c $x $y ($(if ((($x + $y * 3) % 9) -eq 0) { 'j' } else { 'i' })) } }
Bevel $c; Fill $c 0 7 15 8 'K'; Rivets $c 2; Save $c $iron 'chute_encased'
$c = New-Canvas; Metal $c; Bevel $c; Fill $c 0 3 15 3 'K'; Fill $c 0 12 15 12 'K'; Fill $c 6 0 9 15 'D'; Fill $c 0 6 15 9 'D'; Save $c $iron 'chute_intersection'
# smart chute: brass body, filter window
$c = New-Canvas; Metal $c; Bevel $c; Rivets $c 1; Fill $c 4 2 11 6 'K'; Fill $c 5 3 10 5 'y'; Fill $c 3 9 12 13 'D'; Fill $c 4 10 11 12 'x'; Save $c $brass 'smart_chute_side'
$c = New-Canvas; Metal $c; Bevel $c; Rivets $c 1; Fill $c 4 2 11 6 'K'; Fill $c 5 3 10 5 'P'; Fill $c 3 9 12 13 'D'; Fill $c 4 10 11 12 'x'; Save $c $brass 'smart_chute_side_powered'
$c = New-Canvas; Frame $c 3; Rivets $c 1; Save $c $brass 'smart_chute_top'

# --- creative crate -------------------------------------------------------------------------------------
$c = New-Canvas; Metal $c; Bevel $c; Fill $c 1 1 14 2 'D'; Fill $c 1 13 14 14 'D'; for ($i = 2; $i -le 13; $i++) { Set-Px $c $i $i 'L'; Set-Px $c (15 - $i) $i 'L' }; Rivets $c 1; Save $c $creative 'creative_crate_side'
$c = New-Canvas; Metal $c; Bevel $c; Fill $c 4 4 11 11 'K'; Fill $c 5 5 10 10 'y'; Rivets $c 1; Save $c $creative 'creative_crate_top'

# --- item vault: connected textures ---------------------------------------------------------------------
# A tile has a border on every edge that is the outside of the vault (flags L R T B).
function Vault-Base($c, $l, $r, $t, $b) {
    Metal $c
    if ($l) { Fill $c 0 0 0 15 'L'; Fill $c 1 0 1 15 'K' }
    if ($r) { Fill $c 15 0 15 15 'K'; Fill $c 14 0 14 15 'D' }
    if ($t) { Fill $c 0 0 15 0 'L'; Fill $c 0 1 15 1 'K' }
    if ($b) { Fill $c 0 15 15 15 'K'; Fill $c 0 14 15 14 'D' }
    if ($l -and $t) { Set-Px $c 3 3 'R' }; if ($r -and $t) { Set-Px $c 12 3 'R' }
    if ($l -and $b) { Set-Px $c 3 12 'R' }; if ($r -and $b) { Set-Px $c 12 12 'R' }
}
function Ring($c, $cx, $cy, $r0, $r1, [char]$k) {
    for ($y = 0; $y -lt 16; $y++) { for ($x = 0; $x -lt 16; $x++) {
        $d = [math]::Sqrt(($x + .5 - $cx) * ($x + .5 - $cx) + ($y + .5 - $cy) * ($y + .5 - $cy))
        if ($d -ge $r0 -and $d -le $r1) { Set-Px $c $x $y $k }
    } }
}
foreach ($f in 0..15) {
    $l = ($f -band 8) -ne 0; $r = ($f -band 4) -ne 0; $t = ($f -band 2) -ne 0; $b = ($f -band 1) -ne 0
    $flags = "$([int]$l)$([int]$r)$([int]$t)$([int]$b)"
    # side: horizontal bands
    $c = New-Canvas; Vault-Base $c $l $r $t $b; Fill $c 0 6 15 6 'D'; Fill $c 0 9 15 9 'L'; Save $c $vaultPal "item_vault_side_$flags"
    # end, small vault (1x1 / 2x2): a lock dial on a single tile, quarter dials meeting at the centre of a 2x2
    $c = New-Canvas; Vault-Base $c $l $r $t $b
    if ($l -and $r -and $t -and $b) { Ring $c 8 8 3.5 5 'H'; Ring $c 8 8 0 2 'G' }
    elseif (($l -xor $r) -and ($t -xor $b)) {
        $cx = if ($l) { 16 } else { 0 }; $cy = if ($t) { 16 } else { 0 }
        Ring $c $cx $cy 7 9 'H'; Ring $c $cx $cy 0 4 'G'
    }
    Save $c $vaultPal "item_vault_end_$flags"
    # end, large vault (3x3): the dial on the centre tile
    $c = New-Canvas; Vault-Base $c $l $r $t $b
    if (-not ($l -or $r -or $t -or $b)) { Ring $c 8 8 5 7 'H'; Ring $c 8 8 0 3 'G'; Set-Px $c 7 1 'G'; Set-Px $c 8 1 'G' }
    Save $c $vaultPal "item_vault_end_large_$flags"
}

# --- geometry helpers ---------------------------------------------------------------------------------
function UvFace($mat, $u, $v, $w, $h) { [ordered]@{ uv = @($u, $v); uv_size = @($w, $h); material_instance = $mat } }
# a cube with one material instance per face (faces: north south east west up down)
function Cube($origin, $size, $mats, $rotation = $null, $pivot = $null) {
    $sx = $size[0]; $sy = $size[1]; $sz = $size[2]
    $dims = @{ north = @($sx, $sy); south = @($sx, $sy); east = @($sz, $sy); west = @($sz, $sy); up = @($sx, $sz); down = @($sx, $sz) }
    $uv = [ordered]@{}
    foreach ($f in 'north', 'south', 'east', 'west', 'up', 'down') {
        $m = if ($mats -is [string]) { $mats } else { $mats[$f] }
        if (-not $m) { continue }
        $w = [math]::Min(16, $dims[$f][0]); $h = [math]::Min(16, $dims[$f][1])
        $uv[$f] = UvFace $m ([math]::Floor((16 - $w) / 2)) ([math]::Floor((16 - $h) / 2)) $w $h
    }
    $cube = [ordered]@{ origin = $origin; size = $size; uv = $uv }
    if ($rotation) { $cube.rotation = $rotation; $cube.pivot = $pivot }
    $cube
}
function Bone($name, $cubes, $rotation = $null, $pivot = @(0, 0, 0)) {
    $b = [ordered]@{ name = $name; pivot = $pivot; cubes = @($cubes) }
    if ($rotation) { $b.rotation = $rotation }
    $b
}
function Save-Geo($id, $bones) {
    Save-Json "$models/$id.geo.json" ([ordered]@{
        format_version = '1.16.0'
        'minecraft:geometry' = @([ordered]@{
            description = [ordered]@{ identifier = "geometry.levitate.$id"; texture_width = 16; texture_height = 16; visible_bounds_width = 3; visible_bounds_height = 3; visible_bounds_offset = @(0, 0.5, 0) }
            bones = @($bones)
        })
    })
}

# funnel on a wall, opening towards north (the inventory is south of it)
Save-Geo 'funnel_wall' @(
    (Bone 'plate' @((Cube @(-8, 0, 4) @(16, 16, 4) ([ordered]@{ north = 'frame'; south = 'back'; east = 'side'; west = 'side'; up = 'side'; down = 'side' })))),
    (Bone 'hood' @((Cube @(-6, 1, -1) @(12, 13, 5) ([ordered]@{ north = 'frame'; east = 'side'; west = 'side'; up = 'side'; down = 'side' })))),
    (Bone 'flap' @((Cube @(-5, 2, -2) @(10, 11, 1) 'flap'))),
    (Bone 'flap_open' @((Cube @(-5, 2, -2) @(10, 11, 1) 'flap')) @(-22.5, 0, 0) @(0, 13, -1.5))
)
# funnel on the floor, opening up (the inventory is below)
Save-Geo 'funnel_floor' @(
    (Bone 'base' @((Cube @(-8, 0, -8) @(16, 4, 16) ([ordered]@{ north = 'side'; south = 'side'; east = 'side'; west = 'side'; up = 'frame'; down = 'back' })))),
    (Bone 'rim' @((Cube @(-6, 4, -6) @(12, 6, 12) ([ordered]@{ north = 'side'; south = 'side'; east = 'side'; west = 'side'; up = 'frame' }))))
)
# tunnel over a belt along x; walls on north (right) and south (left)
$wallMats = [ordered]@{ north = 'wall'; south = 'wall'; east = 'pillar'; west = 'pillar'; up = 'top'; down = 'pillar' }
Save-Geo 'tunnel' @(
    (Bone 'roof' @((Cube @(-8, 12, -8) @(16, 4, 16) ([ordered]@{ north = 'wall'; south = 'wall'; east = 'top'; west = 'top'; up = 'top'; down = 'pillar' })))),
    (Bone 'pillars' @(
        (Cube @(-8, -8, -8) @(2, 20, 2) 'pillar'), (Cube @(6, -8, -8) @(2, 20, 2) 'pillar'),
        (Cube @(-8, -8, 6) @(2, 20, 2) 'pillar'), (Cube @(6, -8, 6) @(2, 20, 2) 'pillar'))),
    (Bone 'wall_n' @((Cube @(-6, -8, -8) @(12, 20, 2) $wallMats))),
    (Bone 'wall_s' @((Cube @(-6, -8, 6) @(12, 20, 2) $wallMats))),
    (Bone 'flap_w' @((Cube @(-7.5, -6, -6) @(1, 17, 12) 'flap'))),
    (Bone 'flap_e' @((Cube @(6.5, -6, -6) @(1, 17, 12) 'flap'))),
    (Bone 'flap_n' @((Cube @(-6, -6, -7.5) @(12, 17, 1) 'flap'))),
    (Bone 'flap_s' @((Cube @(-6, -6, 6.5) @(12, 17, 1) 'flap')))
)
# chutes
$chuteMats = [ordered]@{ north = 'side'; south = 'side'; east = 'side'; west = 'side'; up = 'top'; down = 'top' }
Save-Geo 'chute' @((Bone 'body' @((Cube @(-6, 0, -6) @(12, 16, 12) $chuteMats))))
Save-Geo 'chute_encased' @((Bone 'body' @((Cube @(-8, 0, -8) @(16, 16, 16) ([ordered]@{ north = 'side'; south = 'side'; east = 'side'; west = 'side'; up = 'top'; down = 'top' })))))
# diagonal chute facing north: the lower half leans towards the chute below and south of it
Save-Geo 'chute_diagonal' @(
    (Bone 'upper' @((Cube @(-6, 8, -6) @(12, 8, 12) $chuteMats))),
    (Bone 'lower' @((Cube @(-6, 0, -6) @(12, 12, 12) $chuteMats @(-45, 0, 0) @(0, 8, 0))))
)
Save-Geo 'smart_chute' @((Bone 'body' @((Cube @(-8, 0, -8) @(16, 16, 16) ([ordered]@{ north = 'side'; south = 'side'; east = 'side'; west = 'side'; up = 'top'; down = 'top' })))))

# --- block definitions ------------------------------------------------------------------------------------
function Mat($texture, $method = 'alpha_test') { [ordered]@{ texture = $texture; render_method = $method } }
function Box($o, $s) { [ordered]@{ origin = $o; size = $s } }
function Block($id, $states, $components, $permutations, $category = 'items') {
    $desc = [ordered]@{ identifier = "levitate:$id"; menu_category = [ordered]@{ category = $category } }
    if ($states) { $desc.states = $states }
    $b = [ordered]@{ description = $desc; components = $components }
    if ($permutations) { $b.permutations = @($permutations) }
    Save-Json "$bpBlocks/$id.json" ([ordered]@{ format_version = '1.21.70'; 'minecraft:block' = $b })
}
function Base($id, $seconds, $resistance, $color) {
    [ordered]@{
        'minecraft:destructible_by_mining' = [ordered]@{ seconds_to_destroy = $seconds }
        'minecraft:destructible_by_explosion' = [ordered]@{ explosion_resistance = $resistance }
        'minecraft:map_color' = $color
        'minecraft:loot' = "loot_tables/blocks/$id.json"
        'minecraft:light_dampening' = 0
    }
}
$yRot = @{ 2 = 0; 3 = 180; 4 = 90; 5 = -90 } # model faces north; Bedrock turns +90 from north to west

# funnels: facing 0..5, extracting, powered, flap (horizontal funnels)
foreach ($n in 'andesite', 'brass') {
    $id = "${n}_funnel"
    $comp = Base $id 0.8 6 ($(if ($n -eq 'brass') { '#d9a941' } else { '#93988d' }))
    $comp['minecraft:geometry'] = [ordered]@{ identifier = 'geometry.levitate.funnel_floor' }
    $comp['minecraft:material_instances'] = [ordered]@{
        '*' = Mat "levitate_${n}_funnel_side"; side = Mat "levitate_${n}_funnel_side"; frame = Mat "levitate_${n}_funnel_frame"
        flap = Mat "levitate_${n}_funnel_flap"; back = Mat "levitate_${n}_funnel_back_extract"
    }
    $comp['minecraft:selection_box'] = Box @(-8, 0, -8) @(16, 10, 16)
    $comp['minecraft:collision_box'] = Box @(-8, 0, -8) @(16, 10, 16)
    $perms = @()
    foreach ($f in 0..5) {
        $pc = [ordered]@{}
        if ($f -le 1) {
            $pc['minecraft:geometry'] = [ordered]@{ identifier = 'geometry.levitate.funnel_floor' }
            if ($f -eq 0) { $pc['minecraft:transformation'] = [ordered]@{ rotation = @(180, 0, 0) } }
        } else {
            $pc['minecraft:geometry'] = [ordered]@{
                identifier = 'geometry.levitate.funnel_wall'
                bone_visibility = [ordered]@{ flap = "q.block_state('levitate:flap') == 0"; flap_open = "q.block_state('levitate:flap') == 1" }
            }
            $pc['minecraft:transformation'] = [ordered]@{ rotation = @(0, $yRot[$f], 0) }
            $pc['minecraft:selection_box'] = Box @(-8, 0, -2) @(16, 16, 10)
            $pc['minecraft:collision_box'] = Box @(-8, 0, -2) @(16, 16, 10)
        }
        $perms += [ordered]@{ condition = "q.block_state('levitate:facing') == $f"; components = $pc }
    }
    foreach ($v in @(@{ c = "q.block_state('levitate:extracting') == 0"; t = 'insert' }, @{ c = "q.block_state('levitate:powered') == 1"; t = 'powered' })) {
        $perms += [ordered]@{ condition = $v.c; components = [ordered]@{ 'minecraft:material_instances' = [ordered]@{
            '*' = Mat "levitate_${n}_funnel_side"; side = Mat "levitate_${n}_funnel_side"; frame = Mat "levitate_${n}_funnel_frame"
            flap = Mat "levitate_${n}_funnel_flap"; back = Mat "levitate_${n}_funnel_back_$($v.t)" } } }
    }
    Block $id ([ordered]@{ 'levitate:facing' = @(0, 1, 2, 3, 4, 5); 'levitate:extracting' = @(0, 1); 'levitate:powered' = @(0, 1); 'levitate:flap' = @(0, 1) }) $comp $perms
}

# tunnels: axis (0 x / 2 z), shape (straight, window, closed, t_left, t_right, cross)
foreach ($n in 'andesite', 'brass') {
    $id = "${n}_tunnel"
    $comp = Base $id 0.8 6 ($(if ($n -eq 'brass') { '#d9a941' } else { '#93988d' }))
    $vis = [ordered]@{
        wall_s = "q.block_state('levitate:shape') <= 2 || q.block_state('levitate:shape') == 4"
        wall_n = "q.block_state('levitate:shape') <= 3"
        flap_s = "q.block_state('levitate:shape') == 3 || q.block_state('levitate:shape') == 5"
        flap_n = "q.block_state('levitate:shape') >= 4"
    }
    $comp['minecraft:geometry'] = [ordered]@{ identifier = 'geometry.levitate.tunnel'; bone_visibility = $vis }
    $mats = [ordered]@{ '*' = Mat "levitate_${n}_tunnel_wall"; wall = Mat "levitate_${n}_tunnel_wall"; top = Mat "levitate_${n}_tunnel_top"; pillar = Mat "levitate_${n}_tunnel_pillar"; flap = Mat "levitate_${n}_tunnel_flap" }
    $comp['minecraft:material_instances'] = $mats
    $comp['minecraft:selection_box'] = Box @(-8, 0, -8) @(16, 16, 16)
    $comp['minecraft:collision_box'] = Box @(-8, 0, -8) @(16, 16, 16)
    $winMats = [ordered]@{}; foreach ($k in $mats.Keys) { $winMats[$k] = $mats[$k] }; $winMats.wall = Mat "levitate_${n}_tunnel_window"
    $perms = @(
        [ordered]@{ condition = "q.block_state('levitate:axis') == 2"; components = [ordered]@{ 'minecraft:transformation' = [ordered]@{ rotation = @(0, -90, 0) } } },
        [ordered]@{ condition = "q.block_state('levitate:shape') == 1"; components = [ordered]@{ 'minecraft:material_instances' = $winMats } }
    )
    Block $id ([ordered]@{ 'levitate:axis' = @(0, 1, 2); 'levitate:shape' = @(0, 1, 2, 3, 4, 5) }) $comp $perms
}

# chute: facing (0 down, 2..5 diagonal), shape (0 intersection, 1 window, 2 normal, 3 encased)
$comp = Base 'chute' 1.0 6 '#a9adb2'
$chuteMat = [ordered]@{ '*' = Mat 'levitate_chute_side'; side = Mat 'levitate_chute_side'; top = Mat 'levitate_chute_top' }
$comp['minecraft:geometry'] = [ordered]@{ identifier = 'geometry.levitate.chute' }
$comp['minecraft:material_instances'] = $chuteMat
$comp['minecraft:selection_box'] = Box @(-6, 0, -6) @(12, 16, 12)
$comp['minecraft:collision_box'] = Box @(-6, 0, -6) @(12, 16, 12)
$perms = @(
    [ordered]@{ condition = "q.block_state('levitate:shape') == 1"; components = [ordered]@{ 'minecraft:material_instances' = [ordered]@{ '*' = Mat 'levitate_chute_window'; side = Mat 'levitate_chute_window'; top = Mat 'levitate_chute_top' } } },
    [ordered]@{ condition = "q.block_state('levitate:shape') == 0"; components = [ordered]@{ 'minecraft:material_instances' = [ordered]@{ '*' = Mat 'levitate_chute_intersection'; side = Mat 'levitate_chute_intersection'; top = Mat 'levitate_chute_top' } } },
    [ordered]@{ condition = "q.block_state('levitate:shape') == 3"; components = [ordered]@{
        'minecraft:geometry' = [ordered]@{ identifier = 'geometry.levitate.chute_encased' }
        'minecraft:material_instances' = [ordered]@{ '*' = Mat 'levitate_chute_encased' 'opaque'; side = Mat 'levitate_chute_encased' 'opaque'; top = Mat 'levitate_chute_top' 'opaque' }
        'minecraft:selection_box' = Box @(-8, 0, -8) @(16, 16, 16); 'minecraft:collision_box' = Box @(-8, 0, -8) @(16, 16, 16) } }
)
foreach ($f in 2..5) {
    $perms += [ordered]@{ condition = "q.block_state('levitate:facing') == $f && q.block_state('levitate:shape') != 3"; components = [ordered]@{
        'minecraft:geometry' = [ordered]@{ identifier = 'geometry.levitate.chute_diagonal' }
        'minecraft:transformation' = [ordered]@{ rotation = @(0, $yRot[$f], 0) } } }
}
Block 'chute' ([ordered]@{ 'levitate:facing' = @(0, 2, 3, 4, 5); 'levitate:shape' = @(2, 0, 1, 3) }) $comp $perms

# smart chute: powered
$comp = Base 'smart_chute' 1.0 6 '#d9a941'
$comp['minecraft:geometry'] = [ordered]@{ identifier = 'geometry.levitate.smart_chute' }
$comp['minecraft:material_instances'] = [ordered]@{ '*' = Mat 'levitate_smart_chute_side'; side = Mat 'levitate_smart_chute_side'; top = Mat 'levitate_smart_chute_top' }
$perms = @([ordered]@{ condition = "q.block_state('levitate:powered') == 1"; components = [ordered]@{ 'minecraft:material_instances' = [ordered]@{
    '*' = Mat 'levitate_smart_chute_side_powered'; side = Mat 'levitate_smart_chute_side_powered'; top = Mat 'levitate_smart_chute_top' } } })
Block 'smart_chute' ([ordered]@{ 'levitate:powered' = @(1, 0) }) $comp $perms

# creative crate
$comp = Base 'creative_crate' 2.0 6 '#a46cc0'
$comp.Remove('minecraft:light_dampening')
$comp['minecraft:geometry'] = 'minecraft:geometry.full_block'
$comp['minecraft:material_instances'] = [ordered]@{ '*' = Mat 'levitate_creative_crate_side' 'opaque'; up = Mat 'levitate_creative_crate_top' 'opaque'; down = Mat 'levitate_creative_crate_top' 'opaque' }
Block 'creative_crate' $null $comp $null

# item vault: axis (0 x / 2 z), connected-texture position len / u / v / large (core::funnels::vaultLook)
function EdgeOf($part, [bool]$low) { if ($part -eq 0) { return $true }; if ($low) { return $part -eq 1 } else { return $part -eq 3 } }
function Flags($l, $r, $t, $b) { "$([int]$l)$([int]$r)$([int]$t)$([int]$b)" }
$comp = Base 'item_vault' 3.0 1200 '#a0a5ab'
$comp.Remove('minecraft:light_dampening')
$comp['minecraft:geometry'] = 'minecraft:geometry.full_block'
$comp['minecraft:material_instances'] = [ordered]@{ '*' = Mat 'levitate_item_vault_side_1111' 'opaque' }
$perms = @()
foreach ($axis in 0, 2) { foreach ($len in 0..3) {
    $combos = @(@{ u = 0; v = 0; large = 0 })
    foreach ($u in 1, 3) { foreach ($v in 1, 3) { $combos += @{ u = $u; v = $v; large = 0 } } }
    foreach ($u in 1..3) { foreach ($v in 1..3) { $combos += @{ u = $u; v = $v; large = 1 } } }
    foreach ($cmb in $combos) {
        $u = $cmb.u; $v = $cmb.v; $lg = $cmb.large
        $endPrefix = if ($lg) { 'levitate_item_vault_end_large_' } else { 'levitate_item_vault_end_' }
        $vt = EdgeOf $v $false; $vb = EdgeOf $v $true
        $first = EdgeOf $len $true; $last = EdgeOf $len $false
        $uLow = EdgeOf $u $true; $uHigh = EdgeOf $u $false
        $m = [ordered]@{}
        if ($axis -eq 0) {
            $m.north = Mat ("levitate_item_vault_side_" + (Flags $last $first $vt $vb)) 'opaque'
            $m.south = Mat ("levitate_item_vault_side_" + (Flags $first $last $vt $vb)) 'opaque'
            $m.up = Mat ("levitate_item_vault_side_" + (Flags $first $last $uLow $uHigh)) 'opaque'
            $m.down = Mat ("levitate_item_vault_side_" + (Flags $first $last $uLow $uHigh)) 'opaque'
            $m.east = Mat ($endPrefix + (Flags $uHigh $uLow $vt $vb)) 'opaque'
            $m.west = Mat ($endPrefix + (Flags $uLow $uHigh $vt $vb)) 'opaque'
        } else {
            $m.east = Mat ("levitate_item_vault_side_" + (Flags $last $first $vt $vb)) 'opaque'
            $m.west = Mat ("levitate_item_vault_side_" + (Flags $first $last $vt $vb)) 'opaque'
            $m.up = Mat ("levitate_item_vault_side_" + (Flags $uLow $uHigh $first $last)) 'opaque'
            $m.down = Mat ("levitate_item_vault_side_" + (Flags $uLow $uHigh $first $last)) 'opaque'
            $m.north = Mat ($endPrefix + (Flags $uHigh $uLow $vt $vb)) 'opaque'
            $m.south = Mat ($endPrefix + (Flags $uLow $uHigh $vt $vb)) 'opaque'
        }
        $m['*'] = $m.north
        $cond = "q.block_state('levitate:axis') == $axis && q.block_state('levitate:len') == $len && q.block_state('levitate:u') == $u && q.block_state('levitate:v') == $v && q.block_state('levitate:large') == $lg"
        $perms += [ordered]@{ condition = $cond; components = [ordered]@{ 'minecraft:material_instances' = $m } }
    }
} }
Block 'item_vault' ([ordered]@{ 'levitate:axis' = @(0, 1, 2); 'levitate:len' = @(0, 1, 2, 3); 'levitate:u' = @(0, 1, 2, 3); 'levitate:v' = @(0, 1, 2, 3); 'levitate:large' = @(0, 1) }) $comp $perms

# --- RP fragments ---------------------------------------------------------------------------------------------
Save-Json (Join-Path $root 'assets/pack/rp/terrain_texture.d/funnels.json') $terrain
$sounds = [ordered]@{}
foreach ($id in 'andesite_funnel', 'brass_funnel', 'andesite_tunnel', 'brass_tunnel', 'chute', 'smart_chute', 'item_vault', 'creative_crate') {
    $sounds["levitate:$id"] = [ordered]@{ sound = $(if ($id -like '*vault*' -or $id -like '*chute*') { 'metal' } elseif ($id -eq 'creative_crate') { 'wood' } else { 'metal' }) }
}
Save-Json (Join-Path $root 'assets/pack/rp/blocks.d/funnels.json') $sounds
Write-Host "funnels textures, models and blocks written ($($terrain.Count) textures)"
