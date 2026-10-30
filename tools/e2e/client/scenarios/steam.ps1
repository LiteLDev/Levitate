# Steam (U18) on the client.
#   local / remote-mod: the selftests pass on the (integrated) server; a 2x2x2 fluid tank shows its
#                       water through the corner windows (client mod), a creative-tank boiler on a
#                       test heater runs a steam engine whose piston moves (two screenshots apart);
#                       a whistle stands on the tank, heated and powered: its tone is one looping
#                       sound (still playing 3 s after the one PlaySound); the boilers show their
#                       gauges on the free sides.
#   remote-vanilla:     the same blocks as static models (no fluid, no piston, no gauges), the
#                       whistle's looping tone and steam jets, without errors.
# Screenshots: out/steam/{steam-a,steam-b,steam-c}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

if (-not $vanilla) {
    $r = Invoke-SelfTest 'steam.*'
    if ($r.Pass -lt 12) { throw "expected at least 12 steam selftests to pass, got $($r.Pass)" }
}

Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1 ~'
Start-Sleep -Milliseconds 500
$mark = Get-LogMark
# a 2x2x2 fluid tank 5 blocks ahead, half full of water
foreach ($dy in 0, 1) { foreach ($dx in 5, 6) { foreach ($dz in -2, -1) { Invoke-Chat "/setblock ~$dx ~$dy ~$dz levitate:fluid_tank" } } }
Start-Sleep -Seconds 2
Invoke-Chat '/create steam ~5 ~ ~-2 fill water 32000'
if (-not $vanilla) { Wait-LogLine 'steam: 32000 mB of minecraft:water moved' 30 -Since $mark | Out-Null }
# a creative tank boiler (4 high) on a kindled test heater, an engine on its west side facing us
Invoke-Chat '/setblock ~6 ~-1 ~2 levitate:steam_test_heater ["levitate:heat"=3]'
foreach ($dy in 0..3) { Invoke-Chat "/setblock ~6 ~$dy ~2 levitate:creative_fluid_tank" }
Invoke-Chat '/setblock ~5 ~1 ~2 levitate:steam_engine ["minecraft:block_face"="west"]'
Invoke-Chat '/setblock ~3 ~1 ~2 levitate:powered_shaft ["levitate:axis"=2]'
# a single tank with an engine on its north side and on top, and a whistle on its west side:
# the models' orientations
Invoke-Chat '/setblock ~6 ~ ~5 levitate:fluid_tank'
Invoke-Chat '/setblock ~6 ~ ~4 levitate:steam_engine ["minecraft:block_face"="north"]'
Invoke-Chat '/setblock ~6 ~1 ~5 levitate:steam_engine ["minecraft:block_face"="up"]'
Invoke-Chat '/setblock ~5 ~ ~5 levitate:steam_whistle ["minecraft:block_face"="west","levitate:size"=2]'
# heat under that tank and redstone next to the whistle: it sounds (one looping tone)
Invoke-Chat '/setblock ~6 ~-1 ~5 levitate:steam_test_heater ["levitate:heat"=3]'
Invoke-Chat '/setblock ~5 ~-1 ~5 minecraft:redstone_block'
Start-Sleep -Seconds 2
Wait-LogLine 'steam: whistle at .* plays levitate.steam.whistle_low as a looping sound for [1-9]' 30 -Since $mark | Out-Null
if (-not $vanilla) {
    Wait-LogLine 'steam: tank at .* is now a boiler' 30 -Since $mark | Out-Null
    Wait-LogLine 'steam: powered shaft at .* turns at 64 RPM' 30 -Since $mark | Out-Null
    Wait-LogLine 'steam client: drawing [1-9]\d* tank fluid\(s\)' 30 | Out-Null
    Wait-LogLine 'steam client: drawing [1-9]\d* engine\(s\)' 30 | Out-Null
    Wait-LogLine 'steam client: drawing the gauges of [1-9]\d* boiler\(s\)' 30 | Out-Null
    # the tone was played once and keeps looping in the game's sound engine
    Wait-LogLine 'steam client: whistle loop still playing after 3 s' 30 | Out-Null
}
Start-Sleep -Seconds 1
Save-Screenshot 'steam-a' | Out-Null
Start-Sleep -Milliseconds 300
Save-Screenshot 'steam-b' | Out-Null
# from above: the tank tops, the whistle and the floor engine
Invoke-Chat '/tp @s ~2 ~5 ~ facing ~5 ~ ~'
Start-Sleep -Seconds 1
Save-Screenshot 'steam-c' | Out-Null
Assert-NoErrors
