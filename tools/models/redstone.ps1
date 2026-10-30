# Models and behavior pack blocks of the U49 additions to the redstone unit (U23): the desk bell
# and the rose quartz lamp. The other redstone blocks are written by hand. Original models (not
# derived from Create's assets). Textures: tools/textures/redstone.ps1 (desk_bell.png,
# rose_quartz_lamp*.png). Sound: tools/sounds/redstone.ps1.
#
#   pwsh tools/models/redstone.ps1
#
# Desk bell: built standing up (+Y) in wheel-style coordinates (p, q in the plane, r up from the
# block centre, a model x is world -x), turned onto `levitate:facing` by the shared facing rotations
# (tools/models/generators.ps1). `levitate:rotor` = 1 hides the bell and the plunger, which the
# client mod draws wobbling (client/redstone/DeskBellRenderer.cpp); vanilla clients see the plunger
# pressed while `levitate:powered`.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$rp = Join-Path $root 'assets/pack/rp'
$bp = Join-Path $root 'assets/pack/bp'
$models = Join-Path $rp 'models/blocks'

function Write-Json($obj, [string]$path, [switch]$Compress) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
    $json = $obj | ConvertTo-Json -Depth 64 -Compress:$Compress
    [IO.File]::WriteAllText($path, $json.Replace("`r`n", "`n") + "`n")
}
function Rnd([double]$v) { [math]::Round($v, 4) }
function Range($n) { @(0..($n - 1)) }

# texture regions of desk_bell.png (32x32)
$regions = @{ base = @(0, 0); bell = @(16, 0); plunger = @(0, 16); knob = @(16, 16) }
function Part($p0, $p1, $q0, $q1, $r0, $r1, $tex) {
    [pscustomobject]@{ p0 = [double]$p0; p1 = [double]$p1; q0 = [double]$q0; q1 = [double]$q1; r0 = [double]$r0; r1 = [double]$r1; tex = $tex }
}
function Lower($part, [double]$by) { Part $part.p0 $part.p1 $part.q0 $part.q1 ($part.r0 - $by) ($part.r1 - $by) $part.tex }
function YCube($part) {
    $sx = $part.p1 - $part.p0; $sy = $part.r1 - $part.r0; $sz = $part.q1 - $part.q0
    $reg = $regions[$part.tex]
    $face = { param($w, $h) [ordered]@{ uv = @($reg[0], $reg[1]); uv_size = @((Rnd ([math]::Min(16, $w))), (Rnd ([math]::Min(16, $h)))) } }
    [ordered]@{
        origin = @((Rnd (-$part.p1)), (Rnd ($part.r0 + 8)), (Rnd $part.q0))
        size   = @((Rnd $sx), (Rnd $sy), (Rnd $sz))
        uv     = [ordered]@{
            north = & $face $sx $sy; south = & $face $sx $sy
            east = & $face $sz $sy; west = & $face $sz $sy
            up = & $face $sx $sz; down = & $face $sx $sz
        }
    }
}
function Bone([string]$name, $parts) { [ordered]@{ name = $name; pivot = @(0, 8, 0); cubes = @($parts | ForEach-Object { YCube $_ }) } }

# Create's DESK_BELL shape: a 10x3x10 base, the bell 8 px across up to 9 px
$base = @((Part -5 5 -5 5 -8 -5 base))
$bell = @(
    (Part -4 4 -4 4 -5 -3 bell), (Part -3.5 3.5 -3.5 3.5 -3 -1.5 bell),
    (Part -2.5 2.5 -2.5 2.5 -1.5 -.5 bell), (Part -1.5 1.5 -1.5 1.5 -.5 0 bell)
)
$plunger = @((Part -.5 .5 -.5 .5 0 .75 plunger), (Part -1 1 -1 1 .75 1.25 knob))
$geo = [ordered]@{
    description = [ordered]@{ identifier = 'geometry.levitate.desk_bell'; texture_width = 32; texture_height = 32; visible_bounds_width = 2; visible_bounds_height = 2; visible_bounds_offset = @(0, 0.5, 0) }
    bones       = @((Bone 'base' $base), (Bone 'bell' $bell), (Bone 'plunger' $plunger), (Bone 'plunger_pressed' @($plunger | ForEach-Object { Lower $_ .75 })))
}
Write-Json ([ordered]@{ format_version = '1.16.0'; 'minecraft:geometry' = @($geo) }) "$models/desk_bell.geo.json" -Compress

# ---- behavior pack ------------------------------------------------------------------------------------------
function BlockFile($id, $category, $states, $components, $permutations, $format = '1.26.0') {
    $desc = [ordered]@{ identifier = "levitate:$id"; menu_category = [ordered]@{ category = $category } }
    $desc.states = $states
    $block = [ordered]@{ description = $desc; components = $components }
    if ($permutations) { $block.permutations = @($permutations) }
    Write-Json ([ordered]@{ format_version = $format; 'minecraft:block' = $block }) "$bp/blocks/$id.json"
}
function Loot($id) { Write-Json ([ordered]@{ pools = @([ordered]@{ rolls = 1; entries = @([ordered]@{ type = 'item'; name = "levitate:$id"; weight = 1 }) }) }) "$bp/loot_tables/blocks/$id.json" }
function Box($o, $s) { [ordered]@{ origin = $o; size = $s } }
$faceNames = @('down', 'up', 'north', 'south', 'west', 'east')
# Y-up models turned to a facing (0 down, 1 up, 2 north, 3 south, 4 west, 5 east), as generators.ps1
$facingRotation = @{ 0 = @(180, 0, 0); 1 = @(0, 0, 0); 2 = @(-90, 0, 0); 3 = @(90, 0, 0); 4 = @(0, 0, 90); 5 = @(0, 0, -90) }

# desk bell
$c = [ordered]@{
    'minecraft:geometry'                  = [ordered]@{ identifier = 'geometry.levitate.desk_bell'; bone_visibility = [ordered]@{
            bell            = "q.block_state('levitate:rotor') != 1"
            plunger         = "q.block_state('levitate:rotor') != 1 && q.block_state('levitate:powered') == 0"
            plunger_pressed = "q.block_state('levitate:rotor') != 1 && q.block_state('levitate:powered') == 1"
        }
    }
    'minecraft:material_instances'        = [ordered]@{ '*' = [ordered]@{ texture = 'levitate_desk_bell'; render_method = 'alpha_test' } }
    'minecraft:collision_box'             = Box @(-4, 0, -4) @(8, 9, 8)
    'minecraft:selection_box'             = Box @(-4, 0, -4) @(8, 9, 8)
    'minecraft:destructible_by_mining'    = [ordered]@{ seconds_to_destroy = 0.5 }
    'minecraft:destructible_by_explosion' = [ordered]@{ explosion_resistance = 0.5 }
    'minecraft:light_dampening'           = 0
    'minecraft:map_color'                 = '#f7e9a3'
    'minecraft:loot'                      = 'loot_tables/blocks/desk_bell.json'
    'minecraft:redstone_conductivity'     = [ordered]@{ redstone_conductor = $false; allows_wire_to_step_down = $true }
    'minecraft:redstone_producer'         = [ordered]@{ power = 0 }
    # Create's ProperWaterloggedBlock
    'minecraft:liquid_detection'          = [ordered]@{ detection_rules = @([ordered]@{ liquid_type = 'water'; can_contain_liquid = $true; on_liquid_touches = 'no_reaction' }) }
}
$perms = @()
foreach ($f in 0..5) { $perms += [ordered]@{ condition = "q.block_state('levitate:facing') == $f"; components = [ordered]@{ 'minecraft:transformation' = [ordered]@{ rotation = $facingRotation[$f] } } } }
# DeskBellBlock.getSignal 15 everywhere while pressed, getDirectSignal into the block it stands on
foreach ($f in 0..5) {
    $support = $faceNames[$f -bxor 1]
    $perms += [ordered]@{
        condition  = "q.block_state('levitate:facing') == $f && q.block_state('levitate:powered') == 1"
        components = [ordered]@{ 'minecraft:redstone_producer' = [ordered]@{ power = 15; strongly_powered_face = $support } }
    }
}
BlockFile 'desk_bell' 'items' ([ordered]@{ 'levitate:facing' = Range 6; 'levitate:powered' = Range 2; 'levitate:rotor' = Range 2 }) $c $perms
Loot 'desk_bell'

# rose quartz lamp (Blocks.REDSTONE_LAMP's properties: strength 0.3, glass sound; light 15 while lit)
$c = [ordered]@{
    'minecraft:geometry'                  = 'minecraft:geometry.full_block'
    'minecraft:material_instances'        = [ordered]@{ '*' = [ordered]@{ texture = 'levitate_rose_quartz_lamp'; render_method = 'opaque' } }
    'minecraft:destructible_by_mining'    = [ordered]@{ seconds_to_destroy = 0.45 }
    'minecraft:destructible_by_explosion' = [ordered]@{ explosion_resistance = 0.3 }
    'minecraft:map_color'                 = '#a04d4e'
    'minecraft:loot'                      = 'loot_tables/blocks/rose_quartz_lamp.json'
    'minecraft:redstone_conductivity'     = [ordered]@{ redstone_conductor = $false; allows_wire_to_step_down = $true }
    'minecraft:redstone_producer'         = [ordered]@{ power = 0 }
}
$perms = @(
    [ordered]@{
        condition  = "q.block_state('levitate:powering') == 1"
        components = [ordered]@{
            'minecraft:material_instances' = [ordered]@{ '*' = [ordered]@{ texture = 'levitate_rose_quartz_lamp_powered'; render_method = 'opaque' } }
            'minecraft:light_emission'     = 15
            'minecraft:redstone_producer'  = [ordered]@{ power = 15 }
        }
    }
)
BlockFile 'rose_quartz_lamp' 'items' ([ordered]@{ 'levitate:powered' = Range 2; 'levitate:powering' = Range 2; 'levitate:activate' = Range 2 }) $c $perms
Loot 'rose_quartz_lamp'

# ---- recipes (Create 6.0.10 crafting/logistics/desk_bell, crafting/kinetics/rose_quartz_lamp) -----------
function Shapeless($id, $ingredients, $unlock) {
    Write-Json ([ordered]@{ format_version = '1.20.10'; 'minecraft:recipe_shapeless' = [ordered]@{
                description = [ordered]@{ identifier = "levitate:$id" }; tags = @('crafting_table')
                ingredients = @($ingredients | ForEach-Object { [ordered]@{ item = $_ } })
                unlock = @([ordered]@{ item = $unlock }); result = [ordered]@{ item = "levitate:$id"; count = 1 }
            } }) "$bp/recipes/$id.json"
}
Shapeless 'desk_bell' @('levitate:andesite_casing', 'levitate:golden_sheet') 'levitate:golden_sheet'
Shapeless 'rose_quartz_lamp' @('levitate:polished_rose_quartz', 'minecraft:redstone', 'levitate:zinc_ingot') 'levitate:polished_rose_quartz'
Write-Host "redstone models and blocks written"
