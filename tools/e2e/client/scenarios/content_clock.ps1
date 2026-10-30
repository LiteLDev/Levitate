# U49 content on a real client: a turning cuckoo clock that cuckoos at noon, a spinning flywheel,
# a desk bell the player rings and two rose quartz lamps (one lit by a redstone block).
#   local / remote-mod: the client mod draws the clock's hands, doors and pig, spins the shaft and
#                       the flywheel and wobbles the bell (log lines of the client renderers).
#   remote-vanilla:     the static models the server's states pick (hands, open doors with the pig,
#                       the pressed plunger, the lit lamp), no errors.
# Screenshots: out/content_clock/{scene,clock-ten,clock-noon-1..7,bell}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

Reset-CameraPerspective
Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set 4000'
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1 ~'
Start-Sleep -Milliseconds 500
$mark0 = Get-LogMark
# cuckoo clock 4 east, its dial towards the player (west), driven from behind
Invoke-Chat '/setblock ~4 ~ ~-1 levitate:cuckoo_clock ["levitate:horizontal_facing"=1]'
Invoke-Chat '/setblock ~5 ~ ~-1 levitate:kinetics_test_source ["levitate:axis"=0]'
# flywheel on an x axis (its face towards the player), driven from behind
Invoke-Chat '/setblock ~6 ~1 ~3 levitate:flywheel ["levitate:axis"=0]'
Invoke-Chat '/setblock ~7 ~1 ~3 levitate:kinetics_test_source ["levitate:axis"=0]'
# desk bell straight ahead
Invoke-Chat '/setblock ~2 ~ ~ levitate:desk_bell ["levitate:facing"=1]'
# two rose quartz lamps; a redstone block lights the far one
Invoke-Chat '/setblock ~4 ~ ~-3 levitate:rose_quartz_lamp'
Invoke-Chat '/setblock ~4 ~ ~-4 levitate:rose_quartz_lamp'
Start-Sleep -Seconds 1
Invoke-Chat '/setblock ~4 ~1 ~-4 redstone_block'
Wait-LogLine 'rose quartz lamp at .* activated' 20 -Since $mark0 | Out-Null
Start-Sleep -Seconds 2
if (-not $vanilla) {
    Wait-LogLine 'kinetics client: drawing \d+ rotating parts' 30 | Out-Null
    Wait-LogLine 'transmission client: drawing \d+ cuckoo clocks' 30 | Out-Null
    Wait-LogLine 'redstone client: drawing \d+ desk bells' 30 | Out-Null
}
Save-Screenshot 'scene' | Out-Null

# 2 blocks in front of the dial, looking at it (east, down): 10 o'clock
Invoke-Chat '/tp @s ~2 ~ ~-1 -90 33'
Start-Sleep -Seconds 1
Save-Screenshot 'clock-ten' | Out-Null
# noon: the pig comes out (doors open around progress 25..40, 40..55, 55..70; with the time
# standing still the clock cuckoos again every 101 ticks): a burst of shots catches it
$mark = Get-LogMark
Invoke-Chat '/time set 6000'
Wait-LogLine 'transmission: cuckoo clock at .* starts the pig animation' 20 -Since $mark | Out-Null
for ($i = 1; $i -le 7; $i++) {
    Save-Screenshot "clock-noon-$i" | Out-Null
    Start-Sleep -Milliseconds 700
}
Invoke-Chat '/time set 4000'
Invoke-Chat '/tp @s ~-2 ~ ~1'
# ring the bell (an item in hand: the game sends the use for any block)
$mark = Get-LogMark
Invoke-Chat '/give @s stick'
Invoke-Chat '/tp @s ~ ~ ~ -90 32'   # the bell 2 blocks east, below eye level
Start-Sleep -Milliseconds 800
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'desk bell at .* rung by' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 150
Save-Screenshot 'bell' | Out-Null
if (-not $vanilla) { Wait-LogLine 'redstone client: desk bell at .* rings' 10 -Since $mark | Out-Null }
Assert-NoErrors
