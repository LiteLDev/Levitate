# Generators (U7) on the client: a scene east of the player with a large water wheel and a water
# wheel driven by bubble columns, a creative motor with a shaft, a hand crank and a valve handle;
# the player turns the crank (held right-click) and the valve handle (one click).
#   local / remote-mod: the selftests pass on the (integrated / BDS) server; the client mod spins
#                       the wheels, shafts and handles (two screenshots apart).
#   remote-vanilla:     static models (each cell of the large wheel on its own block), no errors.
# Screenshots: out/generators/{scene-a,scene-b,crank,valve,motor}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

if (-not $vanilla) {
    $r = Invoke-SelfTest 'generators.*'
    if ($r.Pass -lt 11) { throw "expected at least 11 generators selftests to pass, got $($r.Pass)" }
}

Reset-CameraPerspective
Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1 ~'
$mark0 = Get-LogMark
Start-Sleep -Milliseconds 500
# large water wheel along x (seen face on), bubble columns on its north side
Invoke-Chat '/setblock ~9 ~2 ~ levitate:large_water_wheel ["levitate:axis"=0]'
# (water walled in by glass over soul sand becomes a bubble column)
Invoke-Chat '/fill ~8 ~1 ~-3 ~10 ~4 ~-2 glass'
Invoke-Chat '/fill ~9 ~1 ~-2 ~9 ~3 ~-2 water'
Invoke-Chat '/setblock ~9 ~0 ~-2 soul_sand'
# water wheel facing west, a bubble column north of it
Invoke-Chat '/setblock ~6 ~1 ~3 levitate:water_wheel ["levitate:facing"=4,"levitate:wood"=7]'
Invoke-Chat '/fill ~5 ~1 ~1 ~7 ~2 ~2 glass'
Invoke-Chat '/setblock ~6 ~1 ~2 water'
Invoke-Chat '/setblock ~6 ~0 ~2 soul_sand'
# creative motor facing west into a shaft
Invoke-Chat '/setblock ~6 ~1 ~-3 levitate:creative_motor ["levitate:facing"=4]'
Invoke-Chat '/setblock ~5 ~1 ~-3 levitate:kinetics_test_shaft ["levitate:axis"=0]'
# a hand crank and a red valve handle on the ground
Invoke-Chat '/setblock ~3 ~ ~ levitate:hand_crank ["levitate:facing"=1]'
Invoke-Chat '/setblock ~3 ~ ~-2 levitate:red_valve_handle ["levitate:facing"=1]'
Start-Sleep -Seconds 3
if (-not $vanilla) {
    Wait-LogLine 'kinetics client: drawing \d+ rotating parts' 30 | Out-Null
    Wait-LogLine 'generators client: drawing \d+ handles' 30 | Out-Null
}
Wait-LogLine 'large water wheel at .*: flow score -?\d+, -?4 RPM' 30 -Since $mark0 | Out-Null
Start-Sleep -Seconds 1
Save-Screenshot 'scene-a' | Out-Null
Start-Sleep -Milliseconds 500
Save-Screenshot 'scene-b' | Out-Null

# turn the crank: right-click it a few times (Bedrock repeats a held click)
$mark = Get-LogMark
Invoke-Chat '/tp @s ~ ~ ~ -90 24'   # the crank 3 blocks east, below eye level
Start-Sleep -Milliseconds 600
for ($i = 0; $i -lt 6; $i++) {
    Click 0.5 0.5 -Right
    if ($i -eq 3) { Save-Screenshot 'crank' | Out-Null }
}
Wait-LogLine 'hand crank at .* turned by' 20 -Since $mark | Out-Null

# turn the valve handle once
Start-Sleep -Seconds 1
$mark = Get-LogMark
Invoke-Chat '/tp @s ~ ~ ~ -124 21'  # the valve handle 3 east, 2 north
Start-Sleep -Milliseconds 600
Click 0.5 0.5 -Right
Save-Screenshot 'valve' | Out-Null
Wait-LogLine 'valve handle at .* turned 45 degrees by' 20 -Since $mark | Out-Null

# a close look at the creative motor and its shaft
Invoke-Chat '/tp @s ~3 ~ ~-1 -150 25'
Start-Sleep -Seconds 1
Save-Screenshot 'motor' | Out-Null
Assert-NoErrors
