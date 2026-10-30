# Rotating contraptions (U21) on a real client: a mechanical bearing turning a cross of sails, a
# cart assembler and a controller rail, then the bearings selftests on this world's server.
#   local / remote-mod: the sails turn smoothly (client mod), the bearing's plate turns with them.
#   remote-vanilla:     a client without the mod sees the blocks move as falling-block displays.
# Screenshots: out/bearings/{built,turning-a,turning-b,rails,placed,cart-placing,cart-gone,coupled}.png
$vanilla = $script:Mode -eq 'remote-vanilla'
# Sets a test generator's speed and waits for the server to confirm it. The game sometimes loses
# a typed command or resolves its position oddly: then the chat is pictured and the command sent
# again, through /execute (the position taken from the player on the server).
function Set-GeneratorSpeed([string]$rel, [int]$rpm, [string]$picture) {
    $m = Get-LogMark
    foreach ($try in 1..3) {
        if ($try -eq 1) { Invoke-Chat "/create kinetics $rel speed $rpm" }
        else { Invoke-Chat "/execute as @s at @s run create kinetics $rel speed $rpm" }
        try { Wait-LogLine "Generator at -?\d+ -?\d+ -?\d+ now turns at $rpm " 10 -Since $m | Out-Null; return }
        catch { Save-Screenshot "$picture-try$try" | Out-Null; Write-Step "speed $rpm not confirmed (try $try)" }
    }
    throw "the generator at $rel never turned at $rpm"
}

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
Set-GeneratorSpeed '~9 ~2 ~' 32 'speed-32'
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
Set-GeneratorSpeed '~9 ~2 ~' 0 'speed-0'
if (-not $vanilla) { Wait-LogLine 'mechanical_bearing at .* disassembled' 30 -Since $mark | Out-Null }
Start-Sleep -Milliseconds 1500
Save-Screenshot 'placed' | Out-Null

# a minecart contraption (U21): a minecart placed on a powered controller rail is sent east over
# a powered cart assembler, which puts the plank above it on the cart and sends it on along the
# rails
Invoke-Chat '/setblock ~4 ~ ~-8 levitate:controller_rail ["levitate:rail_axis"=1,"levitate:backwards"=1,"levitate:power"=15]'
Invoke-Chat '/setblock ~4 ~ ~-9 redstone_block'
Invoke-Chat '/setblock ~5 ~ ~-8 levitate:cart_assembler ["levitate:rail_axis"=1,"levitate:rail_type"=0,"levitate:powered"=1]'
Invoke-Chat '/setblock ~5 ~ ~-9 redstone_block'
Invoke-Chat '/setblock ~5 ~1 ~-8 oak_planks'
Invoke-Chat '/fill ~6 ~ ~-8 ~18 ~ ~-8 rail ["rail_direction"=1]'
Invoke-Chat '/give @s minecart 1'
Press-Key 1
# stand south-west of the controller rail and place the cart on it, as a player does
Invoke-Chat '/tp @s ~2 ~ ~-5'
Invoke-Chat '/tp @s ~ ~ ~ -146 23' # at the controller rail (2 east, 3 north, below the eyes)
Start-Sleep -Milliseconds 800
# the plank on the assembler in front of the player, before and after the cart took it away: if
# the structure didn't move on the client, the plank would still be drawn where it was
$before = Save-Screenshot 'cart-placing'
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'minecart placed on levitate:controller_rail' 20 -Since $mark | Out-Null
Wait-LogLine 'cart assembler at .* assembled contraption #\d+ on minecart' 30 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1500
$after = Save-Screenshot 'cart-gone'
Assert-ContraptionMoved $before $after 'the minecart contraption'
Invoke-Chat '/tp @s ~-2 ~ ~5'
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1.5 ~'

# two minecarts coupled with the minecart coupling, as a player does (U21); the client mod draws
# the coupling between them (Create's CouplingRenderer, U38)
Invoke-Chat '/fill ~2 ~ ~8 ~8 ~ ~8 rail ["rail_direction"=1]'
Start-Sleep -Milliseconds 500
Invoke-Chat '/summon minecart ~3.5 ~ ~8.5'
Invoke-Chat '/summon minecart ~6.5 ~ ~8.5'
Invoke-Chat '/give @s levitate:minecart_coupling 4'
Start-Sleep -Milliseconds 800
Press-Key 2
$mark = Get-LogMark
Invoke-Chat '/tp @s ~3.5 ~ ~5.5 facing ~3.5 ~0.4 ~8.5'
Start-Sleep -Milliseconds 800
Click 0.5 0.5 -Relative -Right
Start-Sleep -Milliseconds 600
Invoke-Chat '/tp @s ~ ~ ~ facing ~3 ~0.4 ~3'
Start-Sleep -Milliseconds 800
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'minecarts -?\d+ and -?\d+ coupled' 20 -Since $mark | Out-Null
Invoke-Chat '/tp @s ~1.5 ~1.2 ~0.5 facing ~1.5 ~0.4 ~3'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'coupled' | Out-Null
if (-not $vanilla) { Wait-LogLine 'bearings client: drawing 1 minecart coupling' 20 -Since $mark | Out-Null }
Invoke-Chat '/kill @e[type=minecart]'
Invoke-Chat '/tp @s ~-5 ~-1.2 ~-6'
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1.5 ~'
if (-not $vanilla) {
    $r = Invoke-SelfTest 'bearings.*'
    if ($r.Pass -lt 15) { throw "expected at least 15 bearings selftests to pass, got $($r.Pass)" }
}
Assert-NoErrors
