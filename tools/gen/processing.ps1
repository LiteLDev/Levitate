# Generates the recipe files of the processing units (U13-U16, aligned with Create 6.0.10 in U54):
#
# - the processing recipes Create builds at runtime (RuntimeDataGenerator): planks -> stairs, slab,
#   fence, fence gate, door, trapdoor, pressure plate, button and sign (cutting, 50 ticks) and
#   concrete powder -> concrete (splashing);
# - Create's generated deploying recipes for vanilla copper (adding wax, removing wax, deoxidising);
# - crafting table recipes whose Create ingredient is a tag Bedrock has no counterpart for
#   (`c:stones`: one recipe per stone) and Create's 1 -> 1 "clear" and conversion recipes.
#
#   pwsh tools/gen/processing.ps1
#
# Bedrock item ids differ from Java's for some vanilla items (oak door = wooden_door, waxed copper
# block = waxed_copper, ...); the selftest `assembly.recipe_files` checks every vanilla id in game.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$Generated = [System.Collections.Generic.List[string]]::new()

function Write-Json([string]$rel, $obj) {
    $path = Join-Path $root $rel
    New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
    $json = ($obj | ConvertTo-Json -Depth 64) -replace "`r`n", "`n"
    [System.IO.File]::WriteAllText($path, $json + "`n", [System.Text.UTF8Encoding]::new($false))
    $script:Generated.Add($rel.Replace('\', '/'))
}
function Proc([string]$type, [string]$name, $obj) { Write-Json "assets/recipes/$type/$name.json" $obj }
function Item([string]$id) { [ordered]@{ item = $id } }
function Shaped([string]$name, [string[]]$pattern, $key, [string]$result, [int]$count, [string]$unlock) {
    Write-Json "assets/pack/bp/recipes/$name.json" ([ordered]@{
            format_version          = '1.20.10'
            'minecraft:recipe_shaped' = [ordered]@{
                description = [ordered]@{ identifier = "levitate:$name" }
                tags        = @('crafting_table')
                pattern     = $pattern
                key         = $key
                unlock      = @(Item $unlock)
                result      = [ordered]@{ item = $result; count = $count }
            }
        })
}
function Shapeless([string]$name, $ingredients, [string]$result, [int]$count = 1) {
    Write-Json "assets/pack/bp/recipes/$name.json" ([ordered]@{
            format_version               = '1.20.10'
            'minecraft:recipe_shapeless' = [ordered]@{
                description = [ordered]@{ identifier = "levitate:$name" }
                tags        = @('crafting_table')
                ingredients = @($ingredients)
                unlock      = @($ingredients | Select-Object -First 1)
                result      = [ordered]@{ item = $result; count = $count }
            }
        })
}

$Colours = @('white', 'orange', 'magenta', 'light_blue', 'yellow', 'lime', 'pink', 'gray', 'light_gray', 'cyan', 'purple', 'blue', 'brown', 'green', 'red', 'black')

# ---- cutting: planks -> shapes (RuntimeDataGenerator.cuttingRecipes) --------------------------------------
# Java id suffix -> Bedrock item id for each wood (oak keeps its pre-flattening ids on Bedrock).
$Woods = @('oak', 'spruce', 'birch', 'jungle', 'acacia', 'dark_oak', 'mangrove', 'cherry', 'pale_oak', 'bamboo', 'crimson', 'warped')
$OakIds = @{ fence_gate = 'fence_gate'; door = 'wooden_door'; trapdoor = 'trapdoor'; pressure_plate = 'wooden_pressure_plate'; button = 'wooden_button' }
$Shapes = [ordered]@{ stairs = 1; slab = 2; fence = 1; fence_gate = 1; door = 1; trapdoor = 1; pressure_plate = 1; button = 1; sign = 1 }
foreach ($w in $Woods) {
    foreach ($shape in $Shapes.Keys) {
        $out = if ($w -eq 'oak' -and $OakIds.ContainsKey($shape)) { $OakIds[$shape] } else { "${w}_$shape" }
        $result = [ordered]@{ id = "minecraft:$out" }
        if ($Shapes[$shape] -gt 1) { $result.count = $Shapes[$shape] }
        Proc 'cutting' "${w}_planks_to_${w}_$shape" ([ordered]@{
                type            = 'create:cutting'
                ingredients     = @(Item "minecraft:${w}_planks")
                processing_time = 50
                results         = @($result)
            })
    }
}

# ---- splashing: concrete powder -> concrete (RuntimeDataGenerator.washingRecipes) --------------------------
foreach ($c in $Colours) {
    Proc 'splashing' "${c}_concrete_powder" ([ordered]@{
            type        = 'create:splashing'
            ingredients = @(Item "minecraft:${c}_concrete_powder")
            results     = @([ordered]@{ id = "minecraft:${c}_concrete" })
        })
}

# ---- deploying: vanilla copper (DeployingRecipeGen: addWax / removeWax / deoxidise) ------------------------
# Java names (the recipe file names) and the Bedrock item ids of the 9 copper shapes per stage.
$Stages = @('', 'exposed_', 'weathered_', 'oxidized_')
function CopperId([string]$stage, [string]$shape) {
    if ($shape -eq 'copper_block') { if ($stage -eq '') { return 'copper_block' } return "${stage}copper" }
    return "$stage$shape"
}
foreach ($shape in @('copper_block', 'cut_copper', 'cut_copper_slab', 'cut_copper_stairs', 'chiseled_copper', 'copper_grate', 'copper_door', 'copper_trapdoor', 'copper_bulb')) {
    for ($i = 0; $i -lt 4; $i++) {
        $s = $Stages[$i]
        $plain = CopperId $s $shape
        $javaPlain = if ($shape -eq 'copper_block' -and $s -ne '') { "${s}copper" } else { "$s$shape" }
        $waxed = "waxed_" + $(if ($shape -eq 'copper_block' -and $s -eq '') { 'copper' } else { $plain })
        $javaWaxed = "waxed_$javaPlain"
        Proc 'deploying' "${javaWaxed}_from_adding_wax" ([ordered]@{
                type           = 'create:deploying'
                ingredients    = @((Item "minecraft:$plain"), (Item 'minecraft:honeycomb_block'))
                keep_held_item = $true
                results        = @([ordered]@{ id = "minecraft:$waxed" })
            })
        Proc 'deploying' "${javaPlain}_from_removing_wax" ([ordered]@{
                type           = 'create:deploying'
                ingredients    = @((Item "minecraft:$waxed"), [ordered]@{ tag = 'minecraft:axes' })
                keep_held_item = $true
                results        = @([ordered]@{ id = "minecraft:$plain" })
            })
        if ($i -lt 3) {
            $older = CopperId $Stages[$i + 1] $shape
            Proc 'deploying' "${javaPlain}_from_deoxidising" ([ordered]@{
                    type           = 'create:deploying'
                    ingredients    = @((Item "minecraft:$older"), [ordered]@{ tag = 'minecraft:axes' })
                    keep_held_item = $true
                    results        = @([ordered]@{ id = "minecraft:$plain" })
                })
        }
    }
}

# ---- crafting: `c:stones` ingredients (stone, andesite, diorite, granite, deepslate, tuff) -----------------
# Bedrock has no item tag for this set: one recipe per stone (a grid takes one kind of stone).
$StoneSuffix = [ordered]@{ stone = ''; andesite = '_from_andesite'; diorite = '_from_diorite'; granite = '_from_granite'; deepslate = '_from_deepslate'; tuff = '_from_tuff' }
foreach ($stone in $StoneSuffix.Keys) {
    $S = Item "minecraft:$stone"
    $sfx = $StoneSuffix[$stone]
    Shaped "windmill_bearing$sfx" @('B', 'C', 'I') ([ordered]@{ B = [ordered]@{ tag = 'minecraft:wooden_slabs' }; C = $S; I = Item 'levitate:shaft' }) 'levitate:windmill_bearing' 1 'levitate:shaft'
    Shaped "pulse_extender$sfx" @('  T', 'RCT', 'SSS') ([ordered]@{ T = Item 'minecraft:redstone_torch'; R = Item 'minecraft:redstone'; C = Item 'levitate:brass_sheet'; S = $S }) 'levitate:pulse_extender' 1 'minecraft:redstone_torch'
    Shaped "pulse_repeater$sfx" @('RCT', 'SSS') ([ordered]@{ T = Item 'minecraft:redstone_torch'; R = Item 'minecraft:redstone'; C = Item 'levitate:brass_sheet'; S = $S }) 'levitate:pulse_repeater' 1 'minecraft:redstone_torch'
    Shaped "pulse_timer$sfx" @('RCT', 'SSS') ([ordered]@{ T = Item 'minecraft:redstone_torch'; R = Item 'minecraft:amethyst_shard'; C = Item 'levitate:brass_sheet'; S = $S }) 'levitate:pulse_timer' 1 'minecraft:redstone_torch'
    Shaped "powered_latch$sfx" @(' T ', 'RCR', 'SSS') ([ordered]@{ T = Item 'minecraft:redstone_torch'; R = Item 'minecraft:redstone'; C = Item 'minecraft:lever'; S = $S }) 'levitate:powered_latch' 1 'minecraft:redstone_torch'
    Shaped "powered_toggle_latch$sfx" @(' T ', ' C ', 'SSS') ([ordered]@{ T = Item 'minecraft:redstone_torch'; C = Item 'minecraft:lever'; S = $S }) 'levitate:powered_toggle_latch' 1 'minecraft:redstone_torch'
}

# ---- crafting: Create's 1 -> 1 recipes (a crafted item carries no user data, so "clear" wipes it) ----------
foreach ($id in @('filter', 'attribute_filter', 'schedule')) { Shapeless "${id}_clear" @(Item "levitate:$id") "levitate:$id" }
foreach ($c in $Colours + @('andesite', 'brass', 'copper')) { Shapeless "${c}_table_cloth_clear" @(Item "levitate:${c}_table_cloth") "levitate:${c}_table_cloth" }
foreach ($c in $Colours) { Shapeless "copper_valve_handle_from_$c" @(Item "levitate:${c}_valve_handle") 'levitate:copper_valve_handle' }
Shapeless 'white_sail_from_conversion' @(Item 'levitate:sail_frame') 'levitate:white_sail'
Shapeless 'sail_frame_from_conversion' @(Item 'levitate:white_sail') 'levitate:sail_frame'

# ---- crafting: Create's other shaped recipes --------------------------------------------------------------
Shaped 'mechanical_arm' @('LLA', 'L  ', 'IC ') ([ordered]@{ L = Item 'levitate:brass_sheet'; A = Item 'levitate:andesite_alloy'; I = Item 'levitate:precision_mechanism'; C = Item 'levitate:brass_casing' }) 'levitate:mechanical_arm' 1 'levitate:brass_casing'
Shaped 'mechanical_bearing' @('B', 'C', 'I') ([ordered]@{ B = [ordered]@{ tag = 'minecraft:wooden_slabs' }; C = Item 'levitate:andesite_casing'; I = Item 'levitate:shaft' }) 'levitate:mechanical_bearing' 1 'levitate:andesite_casing'
Shaped 'clockwork_bearing' @('B', 'C', 'S') ([ordered]@{ B = [ordered]@{ tag = 'minecraft:wooden_slabs' }; C = Item 'levitate:brass_casing'; S = Item 'levitate:electron_tube' }) 'levitate:clockwork_bearing' 1 'levitate:brass_casing'
Shaped 'white_sail' @('WS', 'SA') ([ordered]@{ W = [ordered]@{ tag = 'minecraft:wool' }; S = Item 'minecraft:stick'; A = Item 'levitate:andesite_alloy' }) 'levitate:white_sail' 2 'levitate:andesite_alloy'
Shaped 'fluid_pipe_vertical' @('S', 'C', 'S') ([ordered]@{ S = Item 'levitate:copper_sheet'; C = Item 'minecraft:copper_ingot' }) 'levitate:fluid_pipe' 4 'minecraft:copper_ingot'
Shaped 'cake' @(' M ', 'SES', ' P ') ([ordered]@{ M = Item 'minecraft:milk_bucket'; S = Item 'minecraft:sugar'; E = Item 'minecraft:egg'; P = Item 'levitate:dough' }) 'minecraft:cake' 1 'levitate:dough'
Shaped 'chain_from_zinc' @('S', 'C', 'S') ([ordered]@{ S = Item 'levitate:zinc_nugget'; C = Item 'levitate:zinc_ingot' }) 'minecraft:iron_chain' 1 'levitate:zinc_ingot'

# ---- list of generated files (lets the next run remove files that are no longer generated) --------
$listPath = Join-Path $PSScriptRoot 'processing.generated.txt'
if (Test-Path $listPath) {
    foreach ($old in Get-Content $listPath) {
        if ($old -and -not $Generated.Contains($old)) { Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $root $old) }
    }
}
[System.IO.File]::WriteAllText($listPath, (($Generated | Sort-Object) -join "`n") + "`n", [System.Text.UTF8Encoding]::new($false))
Write-Host "processing: $($Generated.Count) files"
