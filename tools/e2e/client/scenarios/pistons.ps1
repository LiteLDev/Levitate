# Linear contraptions (U20) on a real client.
#   local / remote-mod: a rope pulley lowers a platform (the client mod draws the rope and the
#                       magnet down to it), a piston pushes a block out; the selftests pass on the
#                       (integrated) server.
#   remote-vanilla:     the same blocks without the client mod: static models, no errors.
# Screenshots: out/pistons/{built,pulley-moving-1..4,pulley-placed,piston-extended,gantry-moving-1..5,gantry-placed}.png
$vanilla = $script:Mode -eq 'remote-vanilla'
# Two pictures of a moving contraption must differ in a region of the screen (by default right of
# the chat lines, above the hotbar): the structure moves on the client (U34).
function Assert-ContraptionMoved([string]$a, [string]$b, [string]$what, [double]$x0 = 0.42, [double]$x1 = 0.85, [double]$y0 = 0.05, [double]$y1 = 0.85) {
    Add-Type -AssemblyName System.Drawing
    $ia = [System.Drawing.Bitmap]::new($a); $ib = [System.Drawing.Bitmap]::new($b)
    try {
        $changed = 0; $total = 0
        for ($x = [int]($ia.Width * $x0); $x -lt [int]($ia.Width * $x1); $x += 8) {
            for ($y = [int]($ia.Height * $y0); $y -lt [int]($ia.Height * $y1); $y += 8) {
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
# pictures while it goes down (taken at once: the log is read with some delay)
$shots = foreach ($i in 1..4) { Save-Screenshot "pulley-moving-$i"; Start-Sleep -Milliseconds 600 }
Assert-ContraptionMoved $shots[0] $shots[3] 'the lowered block'
Wait-LogLine 'pulley at -?\d+ -?\d+ -?\d+ assembled: \d+ blocks' 90 -Since $mark | Out-Null
Invoke-Chat '/create kinetics ~6 ~6 ~1 speed 96'
if (-not $vanilla) { Wait-LogLine 'pistons: rendering \d+ rope' 30 -Since $mark | Out-Null }
Wait-LogLine 'pulley at -?\d+ -?\d+ -?\d+ disassembled' 120 -Since $mark | Out-Null
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
# pictures while it runs (taken at once: the log is read with some delay)
$shots = foreach ($i in 1..5) { Save-Screenshot "gantry-moving-$i"; Start-Sleep -Milliseconds 800 }
# above the (turning) shafts only
Assert-ContraptionMoved $shots[0] $shots[4] 'the gantry carriage' 0.42 0.85 0.3 0.62
Wait-LogLine 'gantry carriage at -?\d+ -?\d+ -?\d+ assembled: \d+ blocks' 60 -Since $mark | Out-Null
Wait-LogLine 'gantry contraption #\d+ stops at' 90 -Since $mark | Out-Null
Start-Sleep -Seconds 1
Save-Screenshot 'gantry-placed' | Out-Null

if (-not $vanilla) {
    $r = Invoke-SelfTest 'pistons.*'
    if ($r.Pass -lt 12) { throw "expected at least 12 pistons selftests to pass, got $($r.Pass)" }
}
Assert-NoErrors
