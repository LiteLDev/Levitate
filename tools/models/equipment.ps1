# Models and block files of the equipment unit's toolboxes (U31 / U45). Original model (not derived
# from Create's assets). Texture layout: tools/textures/equipment.ps1 (64x64 box UV).
#
#   pwsh tools/models/equipment.ps1
#
# A toolbox is a body with two drawers in front under a lid with a handle; front = +z (south) for
# levitate:facing 0. Block state levitate:toolbox_lid picks the geometry:
#   0  geometry.levitate.toolbox        closed (nobody has it open)
#   1  geometry.levitate.toolbox_open   open: lid turned 135 degrees back, drawers out (Create's
#                                       renderer at rest), set by the server while the screen is open
#   2  geometry.levitate.toolbox_body   the body only: the client mod meshes this for states 0 and 1
#                                       and draws lid and drawers animated (geometry.levitate.toolbox_parts,
#                                       bones lid / drawer); never set in the world
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$rp = Join-Path $root 'assets/pack/rp'
$bp = Join-Path $root 'assets/pack/bp'

$colours = @('white', 'orange', 'magenta', 'light_blue', 'yellow', 'lime', 'pink', 'gray', 'light_gray', 'cyan', 'purple', 'blue', 'brown', 'green', 'red', 'black')
$mapColour = '#7c5a33'

function Write-Json($obj, [string]$path) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
    $json = $obj | ConvertTo-Json -Depth 64
    [IO.File]::WriteAllText($path, $json.Replace("`r`n", "`n") + "`n")
}
function Cube($origin, $size, $uv, $pivot = $null, $rotation = $null) {
    $c = [ordered]@{ origin = $origin; size = $size; uv = $uv }
    if ($rotation) { $c.pivot = $pivot; $c.rotation = $rotation }
    $c
}

# ---- parts (pixels; uv = box UV origin in the 64x64 texture) -------------------------------------------------
$body = @(
    (Cube @(-7, 0, -4) @(14, 4, 8) @(0, 0)),     # base
    (Cube @(-7, 4, -4) @(1, 4, 8) @(0, 12)),     # side walls
    (Cube @(6, 4, -4) @(1, 4, 8) @(18, 12)),
    (Cube @(-6, 4, -4) @(12, 4, 1) @(36, 12))    # back wall
)
# a drawer at height `y`, slid out by `out` pixels
function Drawer([double]$y, [double]$out) {
    @((Cube @(-6, $y, [math]::Round(-3 + $out, 4)) @(12, 2, 7) @(0, 24)), (Cube @(-2, $y, [math]::Round(4 + $out, 4)) @(4, 1, 1) @(38, 24)))
}
# the lid with its handle and latch; turned about its back bottom edge when `deg` is set
$hinge = @(0, 8, -4)
function Lid($deg) {
    $r = if ($deg) { @([double]$deg, 0, 0) } else { $null }
    @(
        (Cube @(-7, 8, -4) @(14, 2, 8) @(0, 34) $hinge $r),
        (Cube @(-3, 10, -1) @(1, 2, 2) @(0, 46) $hinge $r),
        (Cube @(2, 10, -1) @(1, 2, 2) @(8, 46) $hinge $r),
        (Cube @(-3, 12, -1) @(6, 1, 2) @(16, 46) $hinge $r),
        (Cube @(-1, 8, 4) @(2, 2, 1) @(36, 46) $hinge $r)
    )
}
function Geometry([string]$id, $bones) {
    [ordered]@{
        description = [ordered]@{ identifier = $id; texture_width = 64; texture_height = 64; visible_bounds_width = 2; visible_bounds_height = 2.5; visible_bounds_offset = @(0, 0.75, 0) }
        bones       = @($bones)
    }
}
function Bone([string]$name, $cubes) { [ordered]@{ name = $name; pivot = @(0, 0, 0); cubes = @($cubes) } }

# Create's ToolboxRenderer at rest when open: lid 135 degrees back (Bedrock turns a cube by +x the
# other way round from a right-handed turn), lower drawer 0.35 blocks out, upper 0.175
$closed = @($body) + (Drawer 4 0) + (Drawer 6 0) + (Lid $null)
$open = @($body) + (Drawer 4 5.6) + (Drawer 6 2.8) + (Lid 135)
$geo = [ordered]@{
    format_version      = '1.12.0'
    'minecraft:geometry' = @(
        (Geometry 'geometry.levitate.toolbox' (Bone 'box' $closed)),
        (Geometry 'geometry.levitate.toolbox_open' (Bone 'box' $open)),
        (Geometry 'geometry.levitate.toolbox_body' (Bone 'box' $body)),
        (Geometry 'geometry.levitate.toolbox_parts' @((Bone 'lid' (Lid $null)), (Bone 'drawer' (Drawer 4 0))))
    )
}
Write-Json $geo (Join-Path $rp 'models/blocks/toolbox.geo.json')

# ---- the 16 toolbox blocks -----------------------------------------------------------------------------------------
$facingRotation = @(0, -90, 180, 90)
$lidGeometry = @('geometry.levitate.toolbox', 'geometry.levitate.toolbox_open', 'geometry.levitate.toolbox_body')
foreach ($c in $colours) {
    $perms = @()
    for ($f = 0; $f -lt 4; $f++) {
        $perms += [ordered]@{ condition = "q.block_state('levitate:facing') == $f"; components = [ordered]@{ 'minecraft:transformation' = [ordered]@{ rotation = @(0, $facingRotation[$f], 0) } } }
    }
    for ($l = 1; $l -lt 3; $l++) {
        $perms += [ordered]@{ condition = "q.block_state('levitate:toolbox_lid') == $l"; components = [ordered]@{ 'minecraft:geometry' = $lidGeometry[$l] } }
    }
    $block = [ordered]@{
        format_version   = '1.21.70'
        'minecraft:block' = [ordered]@{
            description  = [ordered]@{
                identifier    = "levitate:${c}_toolbox"
                menu_category = [ordered]@{ category = 'equipment' }
                states        = [ordered]@{ 'levitate:facing' = @(0, 1, 2, 3); 'levitate:toolbox_lid' = @(0, 1, 2) }
            }
            components   = [ordered]@{
                'minecraft:geometry'                  = $lidGeometry[0]
                'minecraft:material_instances'        = [ordered]@{ '*' = [ordered]@{ texture = "levitate_${c}_toolbox"; render_method = 'alpha_test' } }
                'minecraft:collision_box'             = [ordered]@{ origin = @(-7, 0, -4); size = @(14, 10, 8) }
                'minecraft:selection_box'             = [ordered]@{ origin = @(-7, 0, -4); size = @(14, 13, 8) }
                'minecraft:destructible_by_mining'    = [ordered]@{ seconds_to_destroy = 1.0 }
                'minecraft:destructible_by_explosion' = [ordered]@{ explosion_resistance = 3 }
                'minecraft:light_dampening'           = 0
                'minecraft:map_color'                 = $mapColour
                'minecraft:loot'                      = "loot_tables/blocks/${c}_toolbox.json"
            }
            permutations = $perms
        }
    }
    Write-Json $block (Join-Path $bp "blocks/${c}_toolbox.json")
}
Write-Host 'equipment models: toolbox geometry and 16 toolbox blocks written'
