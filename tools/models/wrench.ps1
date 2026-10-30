# The wrench held in hand (U8, U42): Create's WrenchItemRenderer turns the small cog on the shaft
# slowly all the time (ScrollValueHandler's passive scroll, 0.5 degrees a tick). Bedrock items have
# no item renderer, so the wrench gets an attachable: its sprite (tools/textures/goggles.ps1)
# extruded into one-pixel cubes, the cog pixels on their own bone turning about the shaft.
#   pwsh -NoProfile -File tools/models/wrench.ps1
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
# the cog's centre (the 'o' pixel) and the shaft direction (grip -> head) in model pixels
$cogX = 8.5 - 8; $cogY = 16 - 9.5

function Pixel($col, $row) {
    $face = [ordered]@{ uv = @($col, $row); uv_size = @(1, 1) }
    [ordered]@{
        origin = @(($col - 8), (15 - $row), -0.5)
        size   = @(1, 1, 1)
        uv     = [ordered]@{ north = $face; south = $face; east = $face; west = $face; up = $face; down = $face }
    }
}

$body = @(); $cog = @()
for ($row = 0; $row -lt 16; $row++) {
    for ($col = 0; $col -lt 16; $col++) {
        $ch = $wrench[$row][$col]
        if ($ch -eq '.') { continue }
        if ($cogPixels -contains [string]$ch) { $cog += Pixel $col $row } else { $body += Pixel $col $row }
    }
}

$geo = [ordered]@{
    format_version      = '1.12.0'
    'minecraft:geometry' = @([ordered]@{
            description = [ordered]@{
                identifier            = 'geometry.levitate.wrench'
                texture_width         = 16
                texture_height        = 16
                visible_bounds_width  = 2
                visible_bounds_height = 2
                visible_bounds_offset = @(0, 1, 0)
            }
            bones       = @(
                [ordered]@{ name = 'wrench'; parent = 'rightItem'; pivot = @(0, 0, 0) }
                [ordered]@{ name = 'model'; parent = 'wrench'; pivot = @(0, 0, 0); cubes = $body }
                # the cog turns about the shaft: a frame tilted onto the shaft, the spinning bone,
                # and the tilt undone for the cubes (given in model coordinates)
                [ordered]@{ name = 'cog_axis'; parent = 'model'; pivot = @($cogX, $cogY, 0); rotation = @(0, 0, -45) }
                [ordered]@{ name = 'cog_spin'; parent = 'cog_axis'; pivot = @($cogX, $cogY, 0) }
                [ordered]@{ name = 'cog'; parent = 'cog_spin'; pivot = @($cogX, $cogY, 0); rotation = @(0, 0, 45); cubes = $cog }
            )
        })
}
$anim = [ordered]@{
    format_version = '1.8.0'
    animations     = [ordered]@{
        # ScrollValueHandler: 0.5 degrees per tick = 10 degrees per second
        'animation.levitate.wrench.cog'               = [ordered]@{ loop = $true; bones = [ordered]@{ cog_spin = [ordered]@{ rotation = @(0, 'q.life_time * 10', 0) } } }
        # held like a tool (Create's wrench model uses the handheld transforms)
        'animation.levitate.wrench.third_person'      = [ordered]@{ loop = $true; bones = [ordered]@{ wrench = [ordered]@{ position = @(1, 9, -1); rotation = @(-90, 0, -45); scale = 0.85 } } }
        'animation.levitate.wrench.first_person'      = [ordered]@{ loop = $true; bones = [ordered]@{ wrench = [ordered]@{ position = @(-1, 11, -2); rotation = @(-55, 160, -5); scale = 1.1 } } }
    }
}
$attachable = [ordered]@{
    format_version         = '1.10.0'
    'minecraft:attachable' = [ordered]@{
        description = [ordered]@{
            identifier         = 'levitate:wrench'
            materials          = [ordered]@{ default = 'entity_alphatest'; enchanted = 'entity_alphatest_glint' }
            textures           = [ordered]@{ default = 'textures/items/wrench'; enchanted = 'textures/misc/enchanted_item_glint' }
            geometry           = [ordered]@{ default = 'geometry.levitate.wrench' }
            animations         = [ordered]@{
                cog          = 'animation.levitate.wrench.cog'
                first_person = 'animation.levitate.wrench.first_person'
                third_person = 'animation.levitate.wrench.third_person'
            }
            scripts            = [ordered]@{ animate = @('cog', [ordered]@{ first_person = 'c.is_first_person' }, [ordered]@{ third_person = '!c.is_first_person' }) }
            render_controllers = @('controller.render.levitate.wrench')
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
Write-Json $attachable "$rp/attachables/wrench.json"
$rc = [ordered]@{
    format_version     = '1.8.0'
    render_controllers = [ordered]@{
        'controller.render.levitate.wrench' = [ordered]@{ geometry = 'Geometry.default'; materials = @([ordered]@{ '*' = 'Material.default' }); textures = @('Texture.default') }
    }
}
Write-Json $rc "$rp/render_controllers/wrench.render_controllers.json"
Write-Host "wrench attachable written ($($body.Count) body pixels, $($cog.Count) cog pixels)"
