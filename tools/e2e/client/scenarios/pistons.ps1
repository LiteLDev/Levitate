# Linear contraptions (U20) on a real client.
#   local / remote-mod: a rope pulley lowers a platform (the client mod draws the rope and the
#                       magnet down to it), a piston pushes a block out; the selftests pass on the
#                       (integrated) server.
#   remote-vanilla:     the same blocks without the client mod: static models, no errors.
# Screenshots: out/pistons/{built,pulley-moving,pulley-placed,piston-extended}.png
$vanilla = $script:Mode -eq 'remote-vanilla'
# Two pictures of a moving contraption must differ around the screen centre (below the chat
# lines): the structure moves on the client (U34).
function Assert-ContraptionMoved([string]$a, [string]$b, [string]$what) {
    Add-Type -AssemblyName System.Drawing
    $ia = [System.Drawing.Bitmap]::new($a); $ib = [System.Drawing.Bitmap]::new($b)
    try {
        $changed = 0; $total = 0
        for ($x = [int]($ia.Width * 0.3); $x -lt [int]($ia.Width * 0.7); $x += 8) {
            for ($y = [int]($ia.Height * 0.32); $y -lt [int]($ia.Height * 0.85); $y += 8) {
                $p = $ia.GetPixel($x, $y); $q = $ib.GetPixel($x, $y); $total++
                if ([Math]::Abs($p.R - $q.R) + [Math]::Abs($p.G - $q.G) + [Math]::Abs($p.B - $q.B) -gt 60) { $changed++ }
            }
        }
    } finally { $ia.Dispose(); $ib.Dispose() }
    Write-Step "${what}: $changed of $total sampled pixels changed between the two pictures"
    if ($changed -lt [Math]::Max(10, $total * 0.01)) { throw "$what did not move on the client ($changed of $total pixels changed)" }
}


Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~2 ~'
Start-Sleep -Milliseconds 500
# rope pulley 6 blocks up, its rope, magnet and a planks platform (without glue only the block
# under the magnet hangs on it, as in Create); generator beside the pulley
Invoke-Chat '/setblock ~6 ~6 ~ levitate:rope_pulley ["levitate:axis"=2]'
Invoke-Chat '/setblock ~6 ~5 ~ levitate:rope'
Invoke-Chat '/setblock ~6 ~4 ~ levitate:pulley_magnet'
Invoke-Chat '/fill ~5 ~3 ~-1 ~7 ~3 ~1 oak_planks'
# a piston facing the player's left with two poles, pushing a gold block
Invoke-Chat '/setblock ~5 ~1 ~-3 levitate:piston_extension_pole ["levitate:facing"=5]'
Invoke-Chat '/setblock ~4 ~1 ~-3 levitate:piston_extension_pole ["levitate:facing"=5]'
Invoke-Chat '/setblock ~6 ~1 ~-3 levitate:mechanical_piston ["levitate:facing"=5,"levitate:axis_first"=0]'
Invoke-Chat '/setblock ~7 ~1 ~-3 gold_block'
Start-Sleep -Seconds 1
Save-Screenshot 'built' | Out-Null

$mark = Get-LogMark
Invoke-Chat '/setblock ~6 ~6 ~1 levitate:kinetics_test_source ["levitate:axis"=2]'
Wait-LogLine 'pulley at -?\d+ -?\d+ -?\d+ assembled: \d+ blocks' 90 -Since $mark | Out-Null
Invoke-Chat '/create kinetics ~6 ~6 ~1 speed 96'
if (-not $vanilla) { Wait-LogLine 'pistons: rendering \d+ rope' 30 -Since $mark | Out-Null }
Start-Sleep -Milliseconds 400
$a = Save-Screenshot 'pulley-moving'
Start-Sleep -Milliseconds 600
$b = Save-Screenshot 'pulley-moving-later'
Assert-ContraptionMoved $a $b 'the lowered platform'
Wait-LogLine 'pulley at -?\d+ -?\d+ -?\d+ disassembled' 90 -Since $mark | Out-Null
Start-Sleep -Seconds 1
Save-Screenshot 'pulley-placed' | Out-Null

# east-facing pistons extend with negative speed (Create's movement modifier)
$mark = Get-LogMark
Invoke-Chat '/setblock ~6 ~1 ~-2 levitate:kinetics_test_source ["levitate:axis"=2]'
Start-Sleep -Seconds 1
Invoke-Chat '/create kinetics ~6 ~1 ~-2 speed -128'
Wait-LogLine 'piston at -?\d+ -?\d+ -?\d+ disassembled at offset 2' 90 -Since $mark | Out-Null
Start-Sleep -Seconds 1
Save-Screenshot 'piston-extended' | Out-Null

# a gantry (U20): a shaft line along x in front of the player, a carriage with a plank on it;
# turning the shafts runs the carriage west towards the start of the line
Invoke-Chat '/tp @s ~ ~ ~ facing ~6 ~2 ~6'
foreach ($dx in 3..9) { Invoke-Chat "/setblock ~$dx ~1 ~6 levitate:gantry_shaft [""levitate:facing""=5]" }
Invoke-Chat '/setblock ~8 ~2 ~6 levitate:gantry_carriage ["levitate:facing"=1,"levitate:axis_first"=0]'
Invoke-Chat '/setblock ~8 ~3 ~6 oak_planks'
Start-Sleep -Seconds 1
$mark = Get-LogMark
Invoke-Chat '/setblock ~2 ~1 ~6 levitate:kinetics_test_source ["levitate:axis"=0]'
Invoke-Chat '/setblock ~10 ~1 ~6 levitate:kinetics_test_source ["levitate:axis"=0]'
Start-Sleep -Seconds 1
Invoke-Chat '/create kinetics ~2 ~1 ~6 speed 64'
Invoke-Chat '/create kinetics ~10 ~1 ~6 speed 64'
Wait-LogLine 'gantry carriage at -?\d+ -?\d+ -?\d+ assembled: \d+ blocks' 60 -Since $mark | Out-Null
Start-Sleep -Milliseconds 600
$a = Save-Screenshot 'gantry-moving'
Start-Sleep -Milliseconds 700
$b = Save-Screenshot 'gantry-moving-later'
Assert-ContraptionMoved $a $b 'the gantry carriage'
Wait-LogLine 'gantry contraption #\d+ stops at' 90 -Since $mark | Out-Null
Start-Sleep -Seconds 1
Save-Screenshot 'gantry-placed' | Out-Null

if (-not $vanilla) {
    $r = Invoke-SelfTest 'pistons.*'
    if ($r.Pass -lt 12) { throw "expected at least 12 pistons selftests to pass, got $($r.Pass)" }
}
Assert-NoErrors
