# The wrench held in hand (U8, U48): Create's WrenchItemRenderer draws the wrench model and turns
# its cog about the shaft by ScrollValueHandler.getScroll: a passive 0.5 degrees a tick while the
# wrench is drawn (plus the jumps of scrolled value boxes, which a vanilla client never sends).
# Bedrock items have no item renderer, so the wrench gets an attachable (it comes from the
# resource pack, so vanilla clients see it too):
# - the body: the item sprite (tools/textures/goggles.ps1) extruded into one-pixel cubes, the cog
#   pixels left out;
# - the cog: our own small gear (a hub on the shaft and eight teeth) across the shaft, on a bone
#   turning about the shaft by q.life_time (10 degrees a second = 0.5 a tick);
# - first and third person need their own geometry, picked by conditional render controller
#   entries (c.is_first_person): in third person the model hangs from the player model's
#   `rightItem` bone; the first-person arm has no such bone, so there the root bone is bound to
#   the slot's hand instead (q.item_slot_to_bone_name, as the vanilla spear and spyglass). An
#   animation controller switches the two poses (as the spear's).
#   pwsh -NoProfile -File tools/models/wrench.ps1
param(
    # third / first person poses: position, rotation (degrees), scale of the root bone
    [double[]]$TpPos = @(1, 9, -1), [double[]]$TpRot = @(-90, 0, -45), [double]$TpScale = 0.85,
    [double[]]$FpPos = @(0, 16, 0), [double[]]$FpRot = @(25, 0, 110), [double]$FpScale = 1.15
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '../..')
$rp   = Join-Path $root 'assets/pack/rp'

# the sprite rows of tools/textures/goggles.ps1 (kept in step with it); c / k / o are the cog
$wrench = @(
    '................',
    '..........dd....',
    '.........daad...',
    '........day.....',
    '........day..dd.',
    '........daaddaad',
    '.........daaaayd',
    '........cdaayyd.',
    '.......ckcdddd..',
    '......ckokc.....',
    '.....wckcc......',
    '....whwc........',
    '...whw..........',
    '..whw...........',
    '.whw............',
    '.ww.............'
)
$cogPixels = 'c', 'k', 'o'

function Face($col, $row) { [ordered]@{ uv = @($col, $row); uv_size = @(1, 1) } }

# The bones of one geometry: the root `wrench` (bound or parented as given), the body pixels, and
# the cog. Model pixels: column `ox` / row `oy` of the sprite at the origin, x right, y up.
function Bones($rootBone, $ox, $oy) {
    # the cog's centre (the 'o' pixel at column 8, row 9); the shaft runs from the grip (bottom
    # left) to the head (top right), at 45 degrees in the sprite plane
    $cx = 8 - $ox + 0.5; $cy = $oy - 9 + 0.5
    $cube = {
        param($origin, $size, $col, $row, $yaw)
        $f = Face $col $row
        $c = [ordered]@{
            origin = $origin
            size   = $size
            uv     = [ordered]@{ north = $f; south = $f; east = $f; west = $f; up = $f; down = $f }
        }
        if ($yaw) { $c.pivot = @($cx, $cy, 0); $c.rotation = @(0, $yaw, 0) }
        $c
    }
    $body = @()
    for ($row = 0; $row -lt 16; $row++) {
        for ($col = 0; $col -lt 16; $col++) {
            $ch = $wrench[$row][$col]
            if ($ch -eq '.' -or $cogPixels -contains [string]$ch) { continue }
            $body += & $cube @(($col - $ox), ($oy - $row), -0.5) @(1, 1, 1) $col $row 0
        }
    }
    # the shaft through the cog (doesn't turn): 1 x 4 px along the shaft
    $shaft = @(& $cube @(($cx - 0.5), ($cy - 2), -0.5) @(1, 4, 1) 8 9 0)
    # the gear (in a frame with the shaft along y): a 2 px hub and eight teeth (two crossing bars,
    # and the same turned by 45 degrees), 1 px thick along the shaft, 5 px across
    $gear = @(
        (& $cube @(($cx - 1), ($cy - 0.5), -1) @(2, 1, 2) 7 9 0),
        (& $cube @(($cx - 2.5), ($cy - 0.5), -0.5) @(5, 1, 1) 8 8 0),
        (& $cube @(($cx - 0.5), ($cy - 0.5), -2.5) @(1, 1, 5) 8 8 0),
        (& $cube @(($cx - 2.5), ($cy - 0.5), -0.5) @(5, 1, 1) 7 9 45),
        (& $cube @(($cx - 0.5), ($cy - 0.5), -2.5) @(1, 1, 5) 7 9 45)
    )
    @(
        $rootBone
        [ordered]@{ name = 'model'; parent = 'wrench'; pivot = @(0, 0, 0); cubes = $body }
        # the cog turns about the shaft: a frame turned onto the shaft (its y along the shaft),
        # the spinning bone, then the shaft and the gear in that frame
        [ordered]@{ name = 'cog_axis'; parent = 'model'; pivot = @($cx, $cy, 0); rotation = @(0, 0, -45); cubes = $shaft }
        [ordered]@{ name = 'cog'; parent = 'cog_axis'; pivot = @($cx, $cy, 0); cubes = $gear }
    )
}

function Geometry($id, $bones) {
    [ordered]@{
        description = [ordered]@{
            identifier            = $id
            texture_width         = 16
            texture_height        = 16
            visible_bounds_width  = 3
            visible_bounds_height = 3
            visible_bounds_offset = @(0, 1, 0)
        }
        bones       = $bones
    }
}
$geo = [ordered]@{
    format_version       = '1.16.0'
    'minecraft:geometry' = @(
        # third person: the sprite centred as an item model, under the player's rightItem bone
        (Geometry 'geometry.levitate.wrench' (Bones ([ordered]@{ name = 'wrench'; parent = 'rightItem'; pivot = @(0, 0, 0) }) 8 15)),
        # first person: the grip at the origin, bound to the hand of the slot
        (Geometry 'geometry.levitate.wrench.first_person' (Bones ([ordered]@{ name = 'wrench'; binding = 'q.item_slot_to_bone_name(c.item_slot)'; pivot = @(0, 0, 0) }) 2 14))
    )
}
$anim = [ordered]@{
    format_version = '1.10.0'
    animations     = [ordered]@{
        # ScrollValueHandler's passive scroll: 0.5 degrees per tick = 10 degrees per second
        'animation.levitate.wrench.cog'          = [ordered]@{ loop = $true; bones = [ordered]@{ cog = [ordered]@{ rotation = @(0, 'q.life_time * 10.0', 0) } } }
        # held like a tool, grip in the hand and the head up and forward
        'animation.levitate.wrench.third_person' = [ordered]@{ loop = $true; bones = [ordered]@{ wrench = [ordered]@{ position = $TpPos; rotation = $TpRot; scale = $TpScale } } }
        'animation.levitate.wrench.first_person' = [ordered]@{ loop = $true; bones = [ordered]@{ wrench = [ordered]@{ position = $FpPos; rotation = $FpRot; scale = $FpScale } } }
    }
}
$controllers = [ordered]@{
    format_version        = '1.10.0'
    animation_controllers = [ordered]@{
        'controller.animation.levitate.wrench.held' = [ordered]@{
            initial_state = 'first_person'
            states        = [ordered]@{
                first_person = [ordered]@{ animations = @('first_person'); transitions = @([ordered]@{ third_person = '!c.is_first_person' }) }
                third_person = [ordered]@{ animations = @('third_person'); transitions = @([ordered]@{ first_person = 'c.is_first_person' }) }
            }
        }
    }
}
$renderControllers = [ordered]@{
    format_version     = '1.8.0'
    render_controllers = [ordered]@{
        'controller.render.levitate.wrench'              = [ordered]@{ geometry = 'Geometry.default'; materials = @([ordered]@{ '*' = 'variable.is_enchanted ? Material.enchanted : Material.default' }); textures = @('Texture.default', 'Texture.enchanted') }
        'controller.render.levitate.wrench.first_person' = [ordered]@{ geometry = 'Geometry.first_person'; materials = @([ordered]@{ '*' = 'variable.is_enchanted ? Material.enchanted : Material.default' }); textures = @('Texture.default', 'Texture.enchanted') }
    }
}
$attachable = [ordered]@{
    format_version         = '1.10.0'
    'minecraft:attachable' = [ordered]@{
        description = [ordered]@{
            identifier         = 'levitate:wrench'
            materials          = [ordered]@{ default = 'entity_alphatest'; enchanted = 'entity_alphatest_glint' }
            textures           = [ordered]@{ default = 'textures/items/wrench'; enchanted = 'textures/misc/enchanted_item_glint' }
            geometry           = [ordered]@{ default = 'geometry.levitate.wrench'; first_person = 'geometry.levitate.wrench.first_person' }
            animations         = [ordered]@{
                cog          = 'animation.levitate.wrench.cog'
                first_person = 'animation.levitate.wrench.first_person'
                third_person = 'animation.levitate.wrench.third_person'
                held         = 'controller.animation.levitate.wrench.held'
            }
            scripts            = [ordered]@{ animate = @('held', 'cog') }
            render_controllers = @(
                [ordered]@{ 'controller.render.levitate.wrench.first_person' = 'c.is_first_person' }
                [ordered]@{ 'controller.render.levitate.wrench' = '!c.is_first_person' }
            )
        }
    }
}

function Write-Json($obj, $path) {
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    $json = $obj | ConvertTo-Json -Depth 20 -Compress
    [IO.File]::WriteAllText($path, $json + "`n", [Text.UTF8Encoding]::new($false))
}
Write-Json $geo "$rp/models/entity/wrench.geo.json"
Write-Json $anim "$rp/animations/wrench.animation.json"
Write-Json $controllers "$rp/animation_controllers/wrench.animation_controllers.json"
Write-Json $renderControllers "$rp/render_controllers/wrench.render_controllers.json"
Write-Json $attachable "$rp/attachables/wrench.json"
Write-Host 'wrench attachable written'
