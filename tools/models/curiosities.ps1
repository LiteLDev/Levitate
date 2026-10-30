# Models, block / item files, attachables and recipes of the equipment unit's curiosities (U31 / U49):
# the peculiar and haunted bells, cardboard armour and the cardboard sword. Original models (not
# derived from Create's assets). Textures: tools/textures/equipment.ps1.
#
#   pwsh tools/models/curiosities.ps1
#
# Bells (Create's PeculiarBellBlock / HauntedBellBlock, vanilla BellBlock states):
#   levitate:facing      FacingID 2..5 (north, south, west, east; 0 / 1 unused): Java's FACING. Floor /
#                        ceiling bells: the player's horizontal direction; wall bells: towards the wall.
#   levitate:attachment  0 floor, 1 ceiling, 2 single wall, 3 double wall (Java's BellAttachType)
#   levitate:powered     Java's POWERED (a redstone signal rang it)
#   levitate:part        0 frame + bell; 1 the frame only: the client mod meshes this one and draws the
#                        bell swinging (geometry.levitate.bell_parts, bone bell); never set in the world
# The models face south (+z) for facing 3, like Create's shapes (forHorizontal(SOUTH)).
#
# Cardboard armour: four armour items (Create's cardboard armour material) with plain armour
# attachables; a hiding wearer (full set, sneaking, not flying) is made invisible by the server and a
# `levitate:cardboard_disguise` entity (a package box) stands in for the player.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$rp = Join-Path $root 'assets/pack/rp'
$bp = Join-Path $root 'assets/pack/bp'

function Write-Json($obj, [string]$path) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
    $json = $obj | ConvertTo-Json -Depth 64
    [IO.File]::WriteAllText($path, $json.Replace("`r`n", "`n") + "`n")
}
# a cube from Java-style block pixels (x, y, z 0..16, model facing south) to Bedrock geometry
function Cube($from, $to, $uv) {
    [ordered]@{ origin = @(($from[0] - 8), $from[1], ($from[2] - 8)); size = @(($to[0] - $from[0]), ($to[1] - $from[1]), ($to[2] - $from[2])); uv = $uv }
}
function Bone([string]$name, $cubes, $pivot = @(0, 0, 0)) { [ordered]@{ name = $name; pivot = $pivot; cubes = @($cubes) } }
function Geometry([string]$id, $bones, [int]$tw = 64, [int]$th = 64) {
    [ordered]@{
        description = [ordered]@{ identifier = $id; texture_width = $tw; texture_height = $th; visible_bounds_width = 2; visible_bounds_height = 2.5; visible_bounds_offset = @(0, 0.75, 0) }
        bones       = @($bones)
    }
}

# ---- bells (64x64 box UV, tools/textures/equipment.ps1) ----------------------------------------------------------
# the bell: a wide lip, the body, a shoulder and a crown loop (pivot: the block centre, Create's rotateCentered)
$bell = @(
    (Cube @(3, 1, 3) @(13, 3, 13) @(0, 0)),     # lip 10x2x10
    (Cube @(4, 3, 4) @(12, 10, 12) @(0, 12)),   # body 8x7x8
    (Cube @(5, 10, 5) @(11, 12, 11) @(32, 12)), # shoulder 6x2x6
    (Cube @(7, 12, 7) @(9, 13, 9) @(56, 12))    # crown 2x1x2
)
$frames = [ordered]@{
    # floor: two posts carry a beam across x
    floor       = @((Cube @(0, 0, 6) @(2, 14, 10) @(0, 28)), (Cube @(14, 0, 6) @(16, 14, 10) @(0, 28)), (Cube @(2, 12, 7) @(14, 14, 9) @(12, 28)))
    # ceiling: a yoke hung from the block above
    ceiling     = @((Cube @(2, 13, 7) @(14, 15, 9) @(12, 28)), (Cube @(2, 15, 7) @(4, 16, 9) @(40, 28)), (Cube @(12, 15, 7) @(14, 16, 9) @(40, 28)))
    # single wall: an arm out of the wall (the wall is +z for facing south)
    single_wall = @((Cube @(7, 13, 6) @(9, 15, 16) @(0, 46)), (Cube @(5, 11, 15) @(11, 16, 16) @(40, 36)))
    # double wall: a bar between two walls
    double_wall = @((Cube @(7, 13, 0) @(9, 15, 16) @(24, 46)), (Cube @(5, 11, 15) @(11, 16, 16) @(40, 36)), (Cube @(5, 11, 0) @(11, 16, 1) @(40, 36)))
}
$attachNames = @('floor', 'ceiling', 'single_wall', 'double_wall')
$geos = @()
foreach ($a in $attachNames) {
    $geos += Geometry "geometry.levitate.bell_$a" @((Bone 'frame' $frames[$a]), (Bone 'bell' $bell @(0, 8, 0)))
    $geos += Geometry "geometry.levitate.bell_${a}_frame" @((Bone 'frame' $frames[$a]))
}
$geos += Geometry 'geometry.levitate.bell_parts' @((Bone 'bell' $bell @(0, 8, 0)))
Write-Json ([ordered]@{ format_version = '1.12.0'; 'minecraft:geometry' = $geos }) (Join-Path $rp 'models/blocks/bell.geo.json')

# selection / collision boxes (Create's AllShapes.BELL_*: frame + the bell (3,1,3)-(13,13,13)), facing south
$boxes = @(
    [ordered]@{ origin = @(-8, 0, -5); size = @(16, 14, 10) },  # floor
    [ordered]@{ origin = @(-6, 1, -5); size = @(12, 15, 10) },  # ceiling
    [ordered]@{ origin = @(-5, 1, -5); size = @(10, 15, 13) },  # single wall
    [ordered]@{ origin = @(-5, 1, -8); size = @(10, 15, 16) }   # double wall
)
# FacingID -> turn of the south-facing model (as the toolboxes: south 0, west -90, north 180, east 90)
$facingRotation = @{ 2 = 180; 3 = 0; 4 = -90; 5 = 90 }
foreach ($b in @(@{ id = 'peculiar_bell'; colour = '#faee4d' }, @{ id = 'haunted_bell'; colour = '#f7e9a3' })) {
    $perms = @()
    foreach ($f in 2..5) {
        $perms += [ordered]@{ condition = "q.block_state('levitate:facing') == $f"; components = [ordered]@{ 'minecraft:transformation' = [ordered]@{ rotation = @(0, $facingRotation[$f], 0) } } }
    }
    for ($a = 0; $a -lt 4; $a++) {
        $perms += [ordered]@{
            condition  = "q.block_state('levitate:attachment') == $a"
            components = [ordered]@{
                'minecraft:geometry'      = "geometry.levitate.bell_$($attachNames[$a])"
                'minecraft:collision_box' = $boxes[$a]
                'minecraft:selection_box' = $boxes[$a]
            }
        }
        $perms += [ordered]@{
            condition  = "q.block_state('levitate:attachment') == $a && q.block_state('levitate:part') == 1"
            components = [ordered]@{ 'minecraft:geometry' = "geometry.levitate.bell_$($attachNames[$a])_frame" }
        }
    }
    $block = [ordered]@{
        format_version    = '1.21.70'
        'minecraft:block' = [ordered]@{
            description  = [ordered]@{
                identifier    = "levitate:$($b.id)"
                menu_category = [ordered]@{ category = 'items' }
                states        = [ordered]@{
                    'levitate:facing'     = @(0, 1, 2, 3, 4, 5)
                    'levitate:attachment' = @(0, 1, 2, 3)
                    'levitate:powered'    = @(0, 1)
                    'levitate:part'       = @(0, 1)
                }
            }
            components   = [ordered]@{
                'minecraft:geometry'                  = 'geometry.levitate.bell_floor'
                'minecraft:material_instances'        = [ordered]@{ '*' = [ordered]@{ texture = "levitate_$($b.id)"; render_method = 'alpha_test' } }
                'minecraft:collision_box'             = $boxes[0]
                'minecraft:selection_box'             = $boxes[0]
                'minecraft:destructible_by_mining'    = [ordered]@{ seconds_to_destroy = 3.0 }
                'minecraft:destructible_by_explosion' = [ordered]@{ explosion_resistance = 6 }
                'minecraft:light_dampening'           = 0
                'minecraft:map_color'                 = $b.colour
                'minecraft:loot'                      = "loot_tables/blocks/$($b.id).json"
            }
            permutations = $perms
        }
    }
    Write-Json $block (Join-Path $bp "blocks/$($b.id).json")
    # Create's loot: the bell itself when it survives the explosion (Bedrock drops of blown up blocks are random anyway)
    Write-Json ([ordered]@{ pools = @([ordered]@{ rolls = 1; entries = @([ordered]@{ type = 'item'; name = "levitate:$($b.id)"; weight = 1 }) }) }) (Join-Path $bp "loot_tables/blocks/$($b.id).json")
}

# ---- cardboard armour and sword -----------------------------------------------------------------------------------
# Create's AllArmorMaterials.CARDBOARD: defense 1 per piece, enchantability 4, no toughness, repaired with
# cardboard; durability ArmorItem.Type.getDurability(4); 1000 ticks of furnace fuel.
$pieces = @(
    @{ id = 'cardboard_helmet'; slot = 'slot.armor.head'; ench = 'armor_head'; durability = 44; geo = 'geometry.humanoid.armor.helmet'; layer = 1; setup = 'variable.helmet_layer_visible = 0.0;' },
    @{ id = 'cardboard_chestplate'; slot = 'slot.armor.chest'; ench = 'armor_torso'; durability = 64; geo = 'geometry.humanoid.armor.chestplate'; layer = 1; setup = 'variable.chest_layer_visible = 0.0;' },
    @{ id = 'cardboard_leggings'; slot = 'slot.armor.legs'; ench = 'armor_legs'; durability = 60; geo = 'geometry.humanoid.armor.leggings'; layer = 2; setup = 'variable.leg_layer_visible = 0.0;' },
    @{ id = 'cardboard_boots'; slot = 'slot.armor.feet'; ench = 'armor_feet'; durability = 52; geo = 'geometry.humanoid.armor.boots'; layer = 1; setup = 'variable.boot_layer_visible = 0.0;' }
)
foreach ($p in $pieces) {
    $item = [ordered]@{
        format_version   = '1.21.70'
        'minecraft:item' = [ordered]@{
            description = [ordered]@{ identifier = "levitate:$($p.id)"; menu_category = [ordered]@{ category = 'equipment' } }
            components  = [ordered]@{
                'minecraft:tags'          = [ordered]@{ tags = @('minecraft:is_armor', 'minecraft:trimmable_armors') }
                'minecraft:icon'          = "levitate_$($p.id)"
                'minecraft:max_stack_size' = 1
                'minecraft:display_name'  = [ordered]@{ value = "item.levitate:$($p.id).name" }
                'minecraft:wearable'      = [ordered]@{ slot = $p.slot; protection = 1 }
                'minecraft:durability'    = [ordered]@{ max_durability = $p.durability }
                'minecraft:enchantable'   = [ordered]@{ slot = $p.ench; value = 4 }
                'minecraft:repairable'    = [ordered]@{ repair_items = @([ordered]@{ items = @('levitate:cardboard'); repair_amount = 'context.other->q.remaining_durability + 0.12 * context.other->q.max_durability' }) }
                'minecraft:fuel'          = [ordered]@{ duration = 50 }
            }
        }
    }
    Write-Json $item (Join-Path $bp "items/$($p.id).json")
    $desc = [ordered]@{
        identifier         = "levitate:$($p.id)"
        materials          = [ordered]@{ default = 'armor'; enchanted = 'armor_enchanted' }
        textures           = [ordered]@{ default = "textures/models/armor/cardboard_$($p.layer)"; enchanted = 'textures/misc/enchanted_item_glint' }
        geometry           = [ordered]@{ default = $p.geo }
        scripts            = [ordered]@{ parent_setup = $p.setup }
        render_controllers = @('controller.render.armor')
    }
    Write-Json ([ordered]@{ format_version = '1.10.0'; 'minecraft:attachable' = [ordered]@{ description = $desc } }) (Join-Path $rp "attachables/$($p.id).json")
}

# The box a hiding player becomes (CardboardArmorHandlerClient): the server makes the player invisible
# (no body, armour or held items on any client) and keeps this entity at the player's feet, turned
# with the player. One of the four plain package boxes (AllPartialModels.PACKAGES_TO_HIDE_AS: U25's
# package geometry and textures) picked at random each time hiding starts (variant 0..3). It bobs
# while the player walks (Create: |cos(t / 2)| x 2 px, at most 5 x the distance moved per tick) and is
# not drawn for a camera closer than 1.8 blocks (Create: not in first person).
$sizes = @('12x12', '10x12', '10x8', '12x10')
$groups = [ordered]@{}; $events = [ordered]@{}
for ($i = 0; $i -lt 4; $i++) {
    $groups["levitate:style_$i"] = [ordered]@{ 'minecraft:variant' = [ordered]@{ value = $i } }
    $events["levitate:style_$i"] = [ordered]@{ add = [ordered]@{ component_groups = @("levitate:style_$i") } }
}
Write-Json ([ordered]@{
        format_version    = '1.21.70'
        'minecraft:entity' = [ordered]@{
            description      = [ordered]@{ identifier = 'levitate:cardboard_disguise'; is_spawnable = $false; is_summonable = $true; is_experimental = $false }
            component_groups = $groups
            components       = [ordered]@{
                'minecraft:collision_box'        = [ordered]@{ width = 0.01; height = 0.01 }
                'minecraft:physics'              = [ordered]@{ has_collision = $false; has_gravity = $false }
                'minecraft:pushable'             = [ordered]@{ is_pushable = $false; is_pushable_by_piston = $false }
                'minecraft:damage_sensor'        = [ordered]@{ triggers = @([ordered]@{ cause = 'all'; deals_damage = 'no' }) }
                'minecraft:health'               = [ordered]@{ value = 1; max = 1 }
                'minecraft:type_family'          = [ordered]@{ family = @('levitate_cardboard_disguise', 'inanimate') }
                'minecraft:fire_immune'          = [ordered]@{}
                'minecraft:knockback_resistance' = [ordered]@{ value = 1 }
                'minecraft:conditional_bandwidth_optimization' = [ordered]@{ default_values = [ordered]@{ max_optimized_distance = 80; max_dropped_ticks = 0; use_motion_prediction_hints = $true } }
            }
            events           = $events
        }
    }) (Join-Path $bp 'entities/cardboard_disguise.json')
$tex = [ordered]@{}; $geo = [ordered]@{}
for ($i = 0; $i -lt 4; $i++) { $tex["s$i"] = "textures/entity/package/cardboard_package_$($sizes[$i])"; $geo["s$i"] = "geometry.levitate.package_$($sizes[$i])" }
Write-Json ([ordered]@{
        format_version          = '1.10.0'
        'minecraft:client_entity' = [ordered]@{
            description = [ordered]@{
                identifier         = 'levitate:cardboard_disguise'
                materials          = [ordered]@{ default = 'entity_alphatest' }
                textures           = $tex
                geometry           = $geo
                animations         = [ordered]@{ bob = 'animation.levitate.cardboard_disguise.bob' }
                scripts            = [ordered]@{ animate = @('bob') }
                render_controllers = @('controller.render.levitate.cardboard_disguise')
            }
        }
    }) (Join-Path $rp 'entity/cardboard_disguise.entity.json')
Write-Json ([ordered]@{
        format_version     = '1.8.0'
        render_controllers = [ordered]@{
            'controller.render.levitate.cardboard_disguise' = [ordered]@{
                arrays          = [ordered]@{
                    geometries = [ordered]@{ 'Array.sizes' = @('Geometry.s0', 'Geometry.s1', 'Geometry.s2', 'Geometry.s3') }
                    textures   = [ordered]@{ 'Array.styles' = @('Texture.s0', 'Texture.s1', 'Texture.s2', 'Texture.s3') }
                }
                geometry        = 'Array.sizes[math.clamp(q.variant, 0, 3)]'
                materials       = @([ordered]@{ '*' = 'Material.default' })
                textures        = @('Array.styles[math.clamp(q.variant, 0, 3)]')
                part_visibility = @([ordered]@{ '*' = 'q.distance_from_camera > 1.8' })
            }
        }
    }) (Join-Path $rp 'render_controllers/cardboard_disguise.render_controllers.json')
Write-Json ([ordered]@{
        format_version = '1.8.0'
        animations     = [ordered]@{
            'animation.levitate.cardboard_disguise.bob' = [ordered]@{
                loop  = $true
                bones = [ordered]@{ box = [ordered]@{ position = @(0, 'q.is_on_ground ? math.min(math.abs(math.cos(q.life_time * 573)) * 2, q.ground_speed * 4) : 0', 0) } }
            }
        }
    }) (Join-Path $rp 'animations/cardboard_disguise.animation.json')

# the sword: Create's CardboardSwordItem (AllToolMaterials.CARDBOARD: 0 uses = unbreakable, +2 damage on a
# sword's 3, Knockback only), stacks to 1, 1000 ticks of fuel
Write-Json ([ordered]@{
        format_version   = '1.21.70'
        'minecraft:item' = [ordered]@{
            description = [ordered]@{ identifier = 'levitate:cardboard_sword'; menu_category = [ordered]@{ category = 'equipment' } }
            components  = [ordered]@{
                'minecraft:tags'           = [ordered]@{ tags = @('minecraft:is_sword') }
                'minecraft:icon'           = 'levitate_cardboard_sword'
                'minecraft:max_stack_size' = 1
                'minecraft:display_name'   = [ordered]@{ value = 'item.levitate:cardboard_sword.name' }
                'minecraft:hand_equipped'  = $true
                'minecraft:damage'         = 5
                'minecraft:enchantable'    = [ordered]@{ slot = 'sword'; value = 1 }
                'minecraft:can_destroy_in_creative' = $false
                'minecraft:fuel'           = [ordered]@{ duration = 50 }
            }
        }
    }) (Join-Path $bp 'items/cardboard_sword.json')

# ---- recipes (Create 6.0.10 generated data) --------------------------------------------------------------------------
function Shaped([string]$name, [string[]]$pattern, $key, [string]$result, [string]$unlock) {
    $k = [ordered]@{}
    foreach ($e in $key.GetEnumerator()) { $k[$e.Key] = [ordered]@{ item = $e.Value } }
    Write-Json ([ordered]@{
            format_version          = '1.20.10'
            'minecraft:recipe_shaped' = [ordered]@{
                description = [ordered]@{ identifier = "levitate:$name" }
                tags        = @('crafting_table')
                pattern     = $pattern
                key         = $k
                unlock      = @([ordered]@{ item = $unlock })
                result      = [ordered]@{ item = $result; count = 1 }
            }
        }) (Join-Path $bp "recipes/$name.json")
}
$cb = @{ P = 'levitate:cardboard' }
Shaped 'cardboard_helmet' @('PPP', 'P P') $cb 'levitate:cardboard_helmet' 'levitate:cardboard'
Shaped 'cardboard_chestplate' @('P P', 'PPP', 'PPP') $cb 'levitate:cardboard_chestplate' 'levitate:cardboard'
Shaped 'cardboard_leggings' @('PPP', 'P P', 'P P') $cb 'levitate:cardboard_leggings' 'levitate:cardboard'
Shaped 'cardboard_boots' @('P P', 'P P') $cb 'levitate:cardboard_boots' 'levitate:cardboard'
Shaped 'cardboard_sword' @('P', 'P', 'S') ([ordered]@{ P = 'levitate:cardboard'; S = 'minecraft:stick' }) 'levitate:cardboard_sword' 'levitate:cardboard'
# c:storage_blocks/brass over c:plates/brass
Shaped 'peculiar_bell' @('I', 'P') ([ordered]@{ I = 'levitate:brass_block'; P = 'levitate:brass_sheet' }) 'levitate:peculiar_bell' 'levitate:brass_sheet'

Write-Host 'curiosities: bell geometry, bells, cardboard armour / sword, attachables and recipes written'
