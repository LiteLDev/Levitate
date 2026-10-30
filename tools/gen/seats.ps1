# Generates the seat dyeing recipes (Create's <colour>_seat_from_other_seat: any seat + a dye gives
# the seat of that colour). Bedrock shapeless recipes can't name "any seat" (seats are block items
# without item tags), so there is one recipe per pair of colours.
#
#   pwsh tools/gen/seats.ps1
param()
$ErrorActionPreference = 'Stop'
$root    = Resolve-Path (Join-Path $PSScriptRoot '../..')
$recipes = Join-Path $root 'assets/pack/bp/recipes'
$colours = 'white', 'orange', 'magenta', 'light_blue', 'yellow', 'lime', 'pink', 'gray', 'light_gray', 'cyan', 'purple', 'blue', 'brown', 'green', 'red', 'black'

Get-ChildItem $recipes -Filter '*_seat_from_*_seat.json' | Remove-Item
$count = 0
foreach ($to in $colours) {
    foreach ($from in $colours) {
        if ($to -eq $from) { continue }
        $id   = "levitate:${to}_seat_from_${from}_seat"
        $json = [ordered]@{
            format_version            = '1.20.10'
            'minecraft:recipe_shapeless' = [ordered]@{
                description = [ordered]@{ identifier = $id }
                tags        = @('crafting_table')
                ingredients = @(
                    [ordered]@{ item = "levitate:${from}_seat" },
                    [ordered]@{ item = "minecraft:${to}_dye" }
                )
                unlock      = @([ordered]@{ item = "levitate:${from}_seat" })
                result      = [ordered]@{ item = "levitate:${to}_seat" }
            }
        }
        $text = ($json | ConvertTo-Json -Depth 8) -replace "`r`n", "`n"
        [IO.File]::WriteAllText((Join-Path $recipes "${to}_seat_from_${from}_seat.json"), $text + "`n")
        ++$count
    }
}
Write-Host "seat dyeing recipes written: $count"
