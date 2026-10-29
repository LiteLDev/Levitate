# Rotating contraptions (U21) on a real client: a mechanical bearing turning a cross of sails, a
# cart assembler and a controller rail, then the bearings selftests on this world's server.
#   local / remote-mod: the sails turn smoothly (client mod), the bearing's plate turns with them.
#   remote-vanilla:     a client without the mod sees the blocks move as falling-block displays.
# Screenshots: out/bearings/{built,turning-a,turning-b,rails,placed}.png
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


Reset-CameraPerspective
Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1.5 ~'
Start-Sleep -Milliseconds 500
# a mechanical bearing facing west (towards the player) on a test source; a cross of sails in front
Invoke-Chat '/setblock ~9 ~2 ~ levitate:kinetics_test_source ["levitate:axis"=0]'
Invoke-Chat '/setblock ~8 ~2 ~ levitate:mechanical_bearing ["levitate:facing"=4]'
Invoke-Chat '/setblock ~7 ~2 ~ levitate:white_sail ["levitate:facing"=4]'
foreach ($o in 1, 2) {
    Invoke-Chat "/setblock ~7 ~$(2 + $o) ~ levitate:red_sail [""levitate:facing""=4]"
    Invoke-Chat "/setblock ~7 ~$(2 - $o) ~ levitate:white_sail [""levitate:facing""=4]"
    Invoke-Chat "/setblock ~7 ~2 ~$o levitate:white_sail [""levitate:facing""=4]"
    Invoke-Chat "/setblock ~7 ~2 ~-$o levitate:white_sail [""levitate:facing""=4]"
}
# rails: a cart assembler on the left, a controller rail on the right
Invoke-Chat '/setblock ~5 ~ ~-4 levitate:cart_assembler ["levitate:rail_axis"=1,"levitate:rail_type"=1]'
Invoke-Chat '/setblock ~5 ~ ~4 levitate:controller_rail ["levitate:rail_axis"=1,"levitate:power"=15]'
Start-Sleep -Seconds 2
Save-Screenshot 'built' | Out-Null

$mark = Get-LogMark
Invoke-Chat '/create kinetics ~9 ~2 ~ speed 32'
if (-not $vanilla) { Wait-LogLine 'mechanical_bearing at .* assembled contraption #\d+ \(9 blocks\)' 30 -Since $mark | Out-Null }
Start-Sleep -Milliseconds 1200
$a = Save-Screenshot 'turning-a'
Start-Sleep -Milliseconds 700
$b = Save-Screenshot 'turning-b'
Assert-ContraptionMoved $a $b 'the turning sails'

Invoke-Chat '/tp @s ~ ~ ~ facing ~5 ~ ~10'
Start-Sleep -Milliseconds 800
Save-Screenshot 'rails' | Out-Null
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1.5 ~'

$mark = Get-LogMark
Invoke-Chat '/create kinetics ~9 ~2 ~ speed 0'
if (-not $vanilla) { Wait-LogLine 'mechanical_bearing at .* disassembled' 30 -Since $mark | Out-Null }
Start-Sleep -Milliseconds 1500
Save-Screenshot 'placed' | Out-Null

# a minecart contraption (U21): a powered controller rail sends a cart east over a powered cart
# assembler, which puts the plank above it on the cart; the cart rolls on along the rails
Invoke-Chat '/tp @s ~ ~ ~ facing ~8 ~ ~-8'
Invoke-Chat '/setblock ~4 ~ ~-8 levitate:controller_rail ["levitate:rail_axis"=1,"levitate:backwards"=1]'
Invoke-Chat '/setblock ~4 ~ ~-9 redstone_block'
Invoke-Chat '/setblock ~5 ~ ~-8 levitate:cart_assembler ["levitate:rail_axis"=1,"levitate:rail_type"=0,"levitate:powered"=1]'
Invoke-Chat '/setblock ~5 ~ ~-9 redstone_block'
Invoke-Chat '/setblock ~5 ~1 ~-8 oak_planks'
Invoke-Chat '/fill ~6 ~ ~-8 ~18 ~ ~-8 rail ["rail_direction"=1]'
Start-Sleep -Seconds 1
$mark = Get-LogMark
Invoke-Chat '/summon minecart ~4.5 ~ ~-7.5'
Wait-LogLine 'cart assembler at .* assembled contraption #\d+ on minecart' 30 -Since $mark | Out-Null
Start-Sleep -Milliseconds 300
$a = Save-Screenshot 'cart-moving'
Start-Sleep -Milliseconds 500
$b = Save-Screenshot 'cart-moving-later'
Assert-ContraptionMoved $a $b 'the minecart contraption'
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1.5 ~'

if (-not $vanilla) {
    $r = Invoke-SelfTest 'bearings.*'
    if ($r.Pass -lt 14) { throw "expected at least 14 bearings selftests to pass, got $($r.Pass)" }
}
Assert-NoErrors
