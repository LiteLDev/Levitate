# Belts (U11) on the client.
#   local / remote-mod: the selftests pass on the (integrated) server; the client mod draws the belt
#                       itself (scrolling band, turning pulleys; two screenshots apart) once it
#                       has the belt speeds.
#   remote-vanilla:     a client without the mod sees the static belt, depot and ejector models.
#   U41: encased sloped and vertical belts, a pulling belt funnel on the belt, and the belt
#        connector preview (dust from a selected shaft towards the shaft looked at; the client mod
#        draws it itself, vanilla clients get it from the server).
# Screenshots: out/belts/{belts-a,belts-b,belts-casing,connector-preview}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

if (-not $vanilla) {
    $r = Invoke-SelfTest 'belts.*'
    if ($r.Pass -lt 26) { throw "expected at least 26 belts selftests to pass, got $($r.Pass)" }
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

# U41: a brass-encased slope and an andesite-encased vertical belt; a pulling belt funnel with its
# chest on the north side of the main belt
Invoke-Chat '/setblock ~3 ~ ~6 levitate:belt ["levitate:facing"=3,"levitate:slope"=1,"levitate:part"=0,"levitate:casing"=2]'
Invoke-Chat '/setblock ~4 ~1 ~6 levitate:belt ["levitate:facing"=3,"levitate:slope"=1,"levitate:part"=2,"levitate:casing"=2]'
Invoke-Chat '/setblock ~7 ~ ~6 levitate:belt ["levitate:facing"=2,"levitate:slope"=3,"levitate:part"=0,"levitate:casing"=1]'
Invoke-Chat '/setblock ~7 ~1 ~6 levitate:belt ["levitate:facing"=2,"levitate:slope"=3,"levitate:part"=2,"levitate:casing"=1]'
Invoke-Chat '/setblock ~5 ~1 ~3 chest'
Invoke-Chat '/setblock ~5 ~1 ~2 levitate:andesite_funnel ["levitate:facing"=2,"levitate:shape"=4]'
Start-Sleep -Seconds 2
Save-Screenshot 'belts-casing' | Out-Null

# the connector preview: two shafts (axis z) 5 blocks apart in front of the player, select the
# left one, then look at the right one
Invoke-Chat '/fill ~2 ~ ~-3 ~9 ~2 ~-1 air'
Invoke-Chat '/setblock ~3 ~1 ~-2 levitate:shaft ["levitate:axis"=2]'
Invoke-Chat '/setblock ~8 ~1 ~-2 levitate:shaft ["levitate:axis"=2]'
Invoke-Chat '/replaceitem entity @s slot.weapon.mainhand 0 levitate:belt_connector 4'
Start-Sleep -Milliseconds 800
Invoke-Chat '/tp @s ~ ~ ~ facing ~3.5 ~1.5 ~-1.5'
Start-Sleep -Milliseconds 600
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'belts: .* selected the shaft at ' 15 -Since $mark | Out-Null
if (-not $vanilla) { Wait-LogLine 'belts client: connector preview from ' 15 -Since $mark | Out-Null }
Invoke-Chat '/tp @s ~ ~ ~ facing ~8.5 ~1.5 ~-1.5'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'connector-preview' | Out-Null
Invoke-Chat '/replaceitem entity @s slot.weapon.mainhand 0 air'
Assert-NoErrors