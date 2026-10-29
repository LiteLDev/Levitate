# Rotating contraptions (U21) on a real client: a mechanical bearing turning a cross of sails, a
# cart assembler and a controller rail, then the bearings selftests on this world's server.
#   local / remote-mod: the sails turn smoothly (client mod), the bearing's plate turns with them.
#   remote-vanilla:     a client without the mod sees the blocks move as falling-block displays.
# Screenshots: out/bearings/{built,turning-a,turning-b,rails,placed}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

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
Save-Screenshot 'turning-a' | Out-Null
Start-Sleep -Milliseconds 700
Save-Screenshot 'turning-b' | Out-Null

Invoke-Chat '/tp @s ~ ~ ~ facing ~5 ~ ~10'
Start-Sleep -Milliseconds 800
Save-Screenshot 'rails' | Out-Null
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1.5 ~'

$mark = Get-LogMark
Invoke-Chat '/create kinetics ~9 ~2 ~ speed 0'
if (-not $vanilla) { Wait-LogLine 'mechanical_bearing at .* disassembled' 30 -Since $mark | Out-Null }
Start-Sleep -Milliseconds 1500
Save-Screenshot 'placed' | Out-Null

if (-not $vanilla) {
    $r = Invoke-SelfTest 'bearings.*'
    if ($r.Pass -lt 14) { throw "expected at least 14 bearings selftests to pass, got $($r.Pass)" }
}
Assert-NoErrors
