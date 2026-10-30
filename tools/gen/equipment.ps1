# Generates the data files of the equipment unit (U31 / U45): the tree fertilizer recipes.
#
#   pwsh tools/gen/equipment.ps1
#
# Create's tree_fertilizer recipe takes two of #minecraft:small_flowers (any two, also two
# different ones), one coral (horn, brain, tube, bubble or fire) and bone meal, and makes 2. A
# Bedrock shapeless ingredient is one item (or an item tag, and vanilla clients only match tags
# they know), so every unordered pair of small flowers gets a recipe per coral.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$recipes = Join-Path $root 'assets/pack/bp/recipes'

# minecraft:small_flowers of Minecraft 1.21.1 (Create 6.0.10)
$Flowers = @('dandelion', 'poppy', 'blue_orchid', 'allium', 'azure_bluet', 'red_tulip', 'orange_tulip', 'white_tulip',
    'pink_tulip', 'oxeye_daisy', 'cornflower', 'lily_of_the_valley', 'wither_rose', 'torchflower')
$Corals = @('horn_coral', 'brain_coral', 'tube_coral', 'bubble_coral', 'fire_coral')

Get-ChildItem $recipes -Filter 'tree_fertilizer_*.json' | Remove-Item
$generated = New-Object System.Collections.Generic.List[string]
for ($i = 0; $i -lt $Flowers.Count; $i++) {
    for ($j = $i; $j -lt $Flowers.Count; $j++) {
        $a = $Flowers[$i]; $b = $Flowers[$j]
        foreach ($coral in $Corals) {
            $name = if ($i -eq $j) { "tree_fertilizer_${a}_$coral" } else { "tree_fertilizer_${a}_${b}_$coral" }
            $ingredients = New-Object System.Collections.Generic.List[object]
            if ($i -eq $j) {
                $ingredients.Add([ordered]@{ item = "minecraft:$a"; count = 2 })
            } else {
                $ingredients.Add([ordered]@{ item = "minecraft:$a" })
                $ingredients.Add([ordered]@{ item = "minecraft:$b" })
            }
            $ingredients.Add([ordered]@{ item = "minecraft:$coral" })
            $ingredients.Add([ordered]@{ item = 'minecraft:bone_meal' })
            $doc = [ordered]@{
                format_version            = '1.20.10'
                'minecraft:recipe_shapeless' = [ordered]@{
                    description = [ordered]@{ identifier = "levitate:$name" }
                    tags        = @('crafting_table')
                    ingredients = $ingredients.ToArray()
                    unlock      = @([ordered]@{ item = "minecraft:$a" })
                    result      = [ordered]@{ item = 'levitate:tree_fertilizer'; count = 2 }
                }
            }
            $path = Join-Path $recipes "$name.json"
            [IO.File]::WriteAllText($path, (($doc | ConvertTo-Json -Depth 8) -replace "`r`n", "`n") + "`n", [Text.UTF8Encoding]::new($false))
            $generated.Add("assets/pack/bp/recipes/$name.json")
        }
    }
}
[IO.File]::WriteAllLines((Join-Path $PSScriptRoot 'equipment.generated.txt'), $generated)
Write-Host "tree fertilizer: $($generated.Count) recipes"
