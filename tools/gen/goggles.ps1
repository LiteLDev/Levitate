# Engineer's goggles recipes (U8): Create's " S " / "GPG" with G = #c:glass_blocks (any glass,
# tinted glass or stained glass, the two lenses may differ). A Bedrock recipe key is one item or
# an item tag and there is no glass tag, so every pair of lenses gets its own recipe.
#   pwsh -NoProfile -File tools/gen/goggles.ps1
$ErrorActionPreference = 'Stop'
$root    = Resolve-Path (Join-Path $PSScriptRoot '../..')
$recipes = Join-Path $root 'assets/pack/bp/recipes'

$colours = 'white', 'orange', 'magenta', 'light_blue', 'yellow', 'lime', 'pink', 'gray', 'light_gray', 'cyan', 'purple', 'blue', 'brown', 'green', 'red', 'black'
$glass   = @('glass', 'tinted_glass') + ($colours | ForEach-Object { "$($_)_stained_glass" })

# drop the files of an earlier run (the plain glass recipe keeps its name)
Get-ChildItem $recipes -Filter 'goggles_*.json' | Remove-Item

# Bedrock shaped recipes match their mirror image too (assume_symmetry), so one recipe per
# unordered pair
for ($i = 0; $i -lt $glass.Count; $i++) {
    for ($j = $i; $j -lt $glass.Count; $j++) {
        $left = $glass[$i]; $right = $glass[$j]
        $plain = $left -eq 'glass' -and $right -eq 'glass'
        $id    = if ($plain) { 'goggles' } else { "goggles_$($left)_$($right)" }
        $key   = [ordered]@{ S = [ordered]@{ item = 'minecraft:string' }; L = [ordered]@{ item = "minecraft:$left" }; P = [ordered]@{ item = 'levitate:golden_sheet' } }
        $pattern = @(' S ', 'LPR')
        if ($left -eq $right) { $pattern = @(' S ', 'LPL') } else { $key['R'] = [ordered]@{ item = "minecraft:$right" } }
        $recipe = [ordered]@{
            format_version          = '1.20.10'
            'minecraft:recipe_shaped' = [ordered]@{
                description = [ordered]@{ identifier = "levitate:$id" }
                tags        = @('crafting_table')
                pattern     = $pattern
                key         = $key
                unlock      = @([ordered]@{ item = 'levitate:golden_sheet' })
                result      = [ordered]@{ item = 'levitate:goggles' }
            }
        }
        $json = $recipe | ConvertTo-Json -Depth 10
        [IO.File]::WriteAllText((Join-Path $recipes "$id.json"), $json.Replace("`r`n", "`n") + "`n", [Text.UTF8Encoding]::new($false))
    }
}
Write-Host "goggles recipes written: $($glass.Count * ($glass.Count + 1) / 2)"
