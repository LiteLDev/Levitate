# Materials (U9): every material / food / ore / decorative stone id is registered, drops, double
# slabs, experience nuggets, Builder's Tea, chromatic compound charge and (in a generated world,
# `-Property level-type=DEFAULT`) zinc ore veins and striated stone layers in fresh chunks.
#
#   pwsh -NoProfile -File tools/e2e/server/run.ps1 -Scenario materials
#   pwsh -NoProfile -File tools/e2e/server/run.ps1 -Scenario materials -Property level-type=DEFAULT
Wait-LogLine 'materials: \d+ blocks ready' 30 | Out-Null
$r = Invoke-SelfTest 'materials.*'
if ($r.Pass -lt 10) { throw "expected at least 10 materials selftests to pass, got $($r.Pass)" }

# Content log: every block / item / recipe of the pack must load without errors.
$bad = Get-ServerLog | Select-String -Pattern '\[(Blocks|Item|Recipes|Molang|Geometry|Texture|Json)\].*(levitate|materials)' |
    Where-Object { $_.Line -match 'ERR|WARN|error' }
if ($bad) { $bad | Select-Object -First 10 | ForEach-Object { Write-Host $_.Line }; throw "content errors for the materials pack" }

# the slab selftests log the player action
if (-not (Get-ServerLog | Select-String 'materials: .* merged .* into a double slab')) { throw 'no double slab log line' }
