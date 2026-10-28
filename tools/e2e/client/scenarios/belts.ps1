# Belts (U11) on the client.
#   local / remote-mod: the selftests pass on the (integrated) server; the client mod draws the belt
#                       itself (scrolling band, turning pulleys; two screenshots apart) once it
#                       has the belt speeds.
#   remote-vanilla:     a client without the mod sees the static belt, depot and ejector models.
# Screenshots: out/belts/{belts-a,belts-b}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

if (-not $vanilla) {
    $r = Invoke-SelfTest 'belts.*'
    if ($r.Pass -lt 12) { throw "expected at least 12 belts selftests to pass, got $($r.Pass)" }
}

Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~-3 ~3'
Start-Sleep -Milliseconds 500
# a 5 block belt along +x two blocks to the right, driven from its start; a depot at its end;
# a dyed sloped piece and an ejector next to it
Invoke-Chat '/setblock ~3 ~ ~1 levitate:kinetics_test_source ["levitate:axis"=2]'
Invoke-Chat '/setblock ~3 ~ ~2 levitate:belt ["levitate:facing"=3,"levitate:part"=0]'
Invoke-Chat '/fill ~4 ~ ~2 ~6 ~ ~2 levitate:belt ["levitate:facing"=3,"levitate:part"=1]'
Invoke-Chat '/setblock ~7 ~ ~2 levitate:belt ["levitate:facing"=3,"levitate:part"=2]'
Invoke-Chat '/setblock ~8 ~ ~2 levitate:depot'
Invoke-Chat '/setblock ~3 ~ ~4 levitate:belt ["levitate:facing"=3,"levitate:slope"=1,"levitate:part"=0,"levitate:dyed"=1,"levitate:color"=14]'
Invoke-Chat '/setblock ~4 ~1 ~4 levitate:belt ["levitate:facing"=3,"levitate:slope"=1,"levitate:part"=2,"levitate:dyed"=1,"levitate:color"=14]'
Invoke-Chat '/setblock ~6 ~ ~4 levitate:weighted_ejector ["levitate:facing"=3]'
Start-Sleep -Seconds 2
Invoke-Chat '/create kinetics ~3 ~ ~1 speed -64'
if (-not $vanilla) {
    Wait-LogLine 'belts client: drawing \d+ belt segments' 30 | Out-Null
}
Start-Sleep -Seconds 1
Save-Screenshot 'belts-a' | Out-Null
Start-Sleep -Milliseconds 300
Save-Screenshot 'belts-b' | Out-Null
Assert-NoErrors