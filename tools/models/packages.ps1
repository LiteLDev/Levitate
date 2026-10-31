# Frogport turning (U56, Create's FrogportBlockEntity.getYaw): the frog faces its target (or the
# player who placed it) in 16 steps of 22.5 degrees, for every client. Bedrock turns a block model
# only in quarter turns (`minecraft:transformation`) and a cube only in 22.5 degree steps up to 45,
# so the frog model exists four times (the base bones `base`, `head`, `goggles` as written by hand,
# and copies turned by +22.5, +45 and -22.5 degrees about the block's vertical axis); the state
# `levitate:yaw` (0..15, step k = k * 22.5 degrees, Create's yaw) shows one copy and turns the
# block by the nearest quarter: yaw = 90 * q + offset, q = floor((k + 1) / 4) mod 4.
#
#   pwsh tools/models/packages.ps1
#
# Rewrites the turned copies in models/blocks/package_frogport.geo.json and the yaw state, bone
# visibility and permutations of blocks/package_frogport.json. A cube rotation value of -t turns
# the cube by t in the world (the model's x is mirrored; see tools/models/generators.ps1).
$ErrorActionPreference = 'Stop'
$root   = (Resolve-Path "$PSScriptRoot/../..").Path
$geo    = Join-Path $root 'assets/pack/rp/models/blocks/package_frogport.geo.json'
$block  = Join-Path $root 'assets/pack/bp/blocks/package_frogport.json'

function Write-Json($obj, [string]$path) {
    $json = $obj | ConvertTo-Json -Depth 64
    [IO.File]::WriteAllText($path, $json.Replace("`r`n", "`n") + "`n")
}

$offsets = @(0, 22.5, 45, -22.5)   # variant v = k mod 4
$stateOf = { param($k) "q.block_state('levitate:yaw') == $k" }
function Steps([scriptblock]$pick) { @(0..15 | Where-Object { & $pick $_ }) }

# ---- geometry -------------------------------------------------------------------------------------
$doc   = Get-Content $geo -Raw | ConvertFrom-Json
$g     = $doc.'minecraft:geometry'[0]
$base  = @($g.bones | Where-Object { $_.name -notmatch '_r\d$' })
$bones = [System.Collections.Generic.List[object]]::new()
foreach ($b in $base) { $bones.Add($b) }
foreach ($v in 1..3) {
    foreach ($b in $base) {
        $copy = $b | ConvertTo-Json -Depth 64 | ConvertFrom-Json
        $copy.name = "$($b.name)_r$v"
        foreach ($c in $copy.cubes) {
            $c | Add-Member -NotePropertyName pivot -NotePropertyValue @(0, $c.origin[1], 0) -Force
            $c | Add-Member -NotePropertyName rotation -NotePropertyValue @(0, (-$offsets[$v]), 0) -Force
        }
        $bones.Add($copy)
    }
}
$g.bones = $bones.ToArray()
Write-Json $doc $geo

# ---- block: state, bone visibility, quarter turns -------------------------------------------------------
$b = Get-Content $block -Raw | ConvertFrom-Json
$desc = $b.'minecraft:block'.description
$desc.states | Add-Member -NotePropertyName 'levitate:yaw' -NotePropertyValue @(0..15) -Force
$vis = [ordered]@{}
foreach ($v in 0..3) {
    $cond = (Steps { param($k) $k % 4 -eq $v } | ForEach-Object { & $stateOf $_ }) -join ' || '
    $suffix = if ($v -eq 0) { '' } else { "_r$v" }
    $vis["base$suffix"]    = $cond
    $vis["head$suffix"]    = $cond
    $vis["goggles$suffix"] = "($cond) && q.block_state('levitate:goggles') == 1"
}
$b.'minecraft:block'.components.'minecraft:geometry'.bone_visibility = $vis
$perms = @()
foreach ($q in 1..3) {
    $cond = (Steps { param($k) ([math]::Floor(($k + 1) / 4) % 4) -eq $q } | ForEach-Object { & $stateOf $_ }) -join ' || '
    $perms += [ordered]@{ condition = $cond; components = [ordered]@{ 'minecraft:transformation' = [ordered]@{ rotation = @(0, (90 * $q), 0) } } }
}
$b.'minecraft:block' | Add-Member -NotePropertyName permutations -NotePropertyValue $perms -Force
Write-Json $b $block
Write-Host "frogport: 4 model copies, 16 yaw steps"
