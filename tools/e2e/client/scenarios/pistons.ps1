# Linear contraptions (U20) on a real client.
#   local / remote-mod: a rope pulley lowers a platform (the client mod draws the rope and the
#                       magnet down to it), a piston pushes a block out; the selftests pass on the
#                       (integrated) server.
#   remote-vanilla:     the same blocks without the client mod: static models, no errors.
# Screenshots: out/pistons/{built,pulley-moving,pulley-placed,piston-extended}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

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
Save-Screenshot 'pulley-moving' | Out-Null
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

if (-not $vanilla) {
    $r = Invoke-SelfTest 'pistons.*'
    if ($r.Pass -lt 12) { throw "expected at least 12 pistons selftests to pass, got $($r.Pass)" }
}
Assert-NoErrors
