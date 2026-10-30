# Bogey wheel animation steps (U38, Create's BogeyRenderer: the wheels turn with the distance the
# carriage travels). Writes bones spokes0..spokes3 into the bogey models: a brass cross on the
# outer face of every wheel, turned a further 22.5 degrees about the axle per step. The client mod
# shows a carried bogey with `levitate:anim` 0..3 while it draws it (src/client/trains/
# BogeyRenderer.cpp); the cross is 4-fold symmetric, so four steps make a full turn. Block geometry
# allows cube rotations of -45..45 degrees and must stay within 30 px, so the rims stay still
# (square discs) and only the crosses turn; the fourth step uses -22.5 (= 67.5 for a cross).
#
#   pwsh tools/models/bogeys.ps1      (idempotent: replaces earlier spokes bones)
param()
$ErrorActionPreference = 'Stop'
$root   = Resolve-Path (Join-Path $PSScriptRoot '../..')
$models = Join-Path $root 'assets/pack/rp/models/blocks'
$steps  = @(0, 22.5, 45, -22.5)

# the brass rivet pixel of textures/blocks/bogey.png (tools/textures/trains.ps1)
function Face { [ordered]@{ uv = @(2, 2); uv_size = @(1, 1) } }
function Uv { [ordered]@{ north = (Face); south = (Face); east = (Face); west = (Face); up = (Face); down = (Face) } }

foreach ($name in 'small_bogey', 'large_bogey') {
    $path  = Join-Path $models "$name.geo.json"
    $geo   = Get-Content $path -Raw | ConvertFrom-Json
    $g     = $geo.'minecraft:geometry'[0]
    $kept  = @($g.bones | Where-Object { $_.name -notlike 'spokes*' })
    $wheel = $g.bones | Where-Object { $_.name -eq 'wheels' }
    if (-not $wheel) { throw "$name has no wheels bone" }
    $discs = @($wheel.cubes | Where-Object { [double]$_.size[2] -le 2 -and [double]$_.size[0] -ge 8 })
    $bones = [System.Collections.ArrayList]::new()
    foreach ($b in $kept) { [void]$bones.Add($b) }
    for ($i = 0; $i -lt 4; ++$i) {
        $cubes = foreach ($d in $discs) {
            $cx = [double]$d.origin[0] + [double]$d.size[0] / 2
            $cy = [double]$d.origin[1] + [double]$d.size[1] / 2
            $z0 = [double]$d.origin[2]
            # the outer face: away from the bogey's centre line
            $zf = if ($z0 -lt 0) { $z0 - 0.5 } else { $z0 + [double]$d.size[2] }
            $arm = [double]$d.size[0] / 2 - 1
            foreach ($bar in @(@(($cx - $arm), ($cy - 0.5), (2 * $arm), 1), @(($cx - 0.5), ($cy - $arm), 1, (2 * $arm)))) {
                $cube = [ordered]@{
                    origin = @($bar[0], $bar[1], $zf)
                    size   = @($bar[2], $bar[3], 0.5)
                    uv     = (Uv)
                }
                if ($steps[$i] -ne 0) {
                    $cube.pivot    = @($cx, $cy, ($zf + 0.25))
                    $cube.rotation = @(0, 0, $steps[$i])
                }
                [pscustomobject]$cube
            }
        }
        [void]$bones.Add([pscustomobject][ordered]@{ name = "spokes$i"; pivot = @(0, 0, 0); cubes = @($cubes) })
    }
    $g.bones = $bones.ToArray()
    $text = ($geo | ConvertTo-Json -Depth 32) -replace "`r`n", "`n"
    [IO.File]::WriteAllText($path, $text + "`n")
    Write-Host "$name.geo.json: spokes0..3 written ($($discs.Count) wheels)"
}
