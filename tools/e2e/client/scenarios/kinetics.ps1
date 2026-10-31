# Kinetic network (U2) on the client: rotating parts.
#   local / remote-mod: the client mod draws the rotating parts spinning (two screenshots apart),
#                       meshing cogs line up; the selftests pass on the (integrated) server.
#   remote-vanilla:     a client without the mod sees the static models, without errors.
#   local / remote-mod (U52): overstressing the network flashes its rotating parts red, recovering
#                       flashes them green (Create's KineticEffectHandler).
# Screenshots: out/kinetics/{kinetics-a,kinetics-b}.png, kinetics-overstressed.png, kinetics-recovered.png
$vanilla = $script:Mode -eq 'remote-vanilla'

if (-not $vanilla) {
    $r = Invoke-SelfTest 'kinetics.*'
    if ($r.Pass -lt 10) { throw "expected at least 10 kinetics selftests to pass, got $($r.Pass)" }
}

# Look along +x at a wall of cogs (axis x) 5 blocks ahead.
Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1.6 ~'
Start-Sleep -Milliseconds 500
Invoke-Chat '/setblock ~6 ~1 ~ levitate:kinetics_test_source ["levitate:axis"=0]'
Invoke-Chat '/setblock ~5 ~1 ~ levitate:kinetics_test_cog ["levitate:axis"=0]'
Invoke-Chat '/setblock ~5 ~1 ~1 levitate:kinetics_test_cog ["levitate:axis"=0]'
Invoke-Chat '/setblock ~5 ~2 ~ levitate:kinetics_test_cog ["levitate:axis"=0]'
Invoke-Chat '/setblock ~5 ~3 ~1 levitate:kinetics_test_large_cog ["levitate:axis"=0]'
Invoke-Chat '/setblock ~4 ~1 ~-1 levitate:kinetics_test_shaft ["levitate:axis"=0]'
Invoke-Chat '/setblock ~5 ~1 ~-1 levitate:kinetics_test_cog ["levitate:axis"=0]'
Start-Sleep -Seconds 2
if (-not $vanilla) {
    Invoke-Chat '/create kinetics ~6 ~1 ~ speed 32'
    Wait-LogLine 'kinetics client: server has the kinetic sync' 30 | Out-Null
    Wait-LogLine 'kinetics client: drawing \d+ rotating parts' 30 | Out-Null
}
Start-Sleep -Seconds 1
Save-Screenshot 'kinetics-a' | Out-Null
Start-Sleep -Milliseconds 400
Save-Screenshot 'kinetics-b' | Out-Null
if (-not $vanilla) {
    # 3 consumers behind the source: 3 x 4 SU/RPM > 8 SU/RPM
    $mark = Get-LogMark
    foreach ($dx in 7, 8, 9) { Invoke-Chat "/setblock ~$dx ~1 ~ levitate:kinetics_test_consumer [`"levitate:axis`"=0]" }
    Wait-LogLine 'kinetics client: overstress flash \(red\)' 30 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 150
    Save-Screenshot 'kinetics-overstressed' | Out-Null
    $mark = Get-LogMark
    foreach ($dx in 7, 8, 9) { Invoke-Chat "/setblock ~$dx ~1 ~ air" }
    Wait-LogLine 'kinetics client: overstress flash \(green\)' 30 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 150
    Save-Screenshot 'kinetics-recovered' | Out-Null
}
Assert-NoErrors