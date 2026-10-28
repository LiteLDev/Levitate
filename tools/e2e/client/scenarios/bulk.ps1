# Bulk processing (U13) on the client: millstone, a crushing wheel pair, an encased fan blowing at
# the camera (airflow particles) and a fan with a nozzle, all turning.
#   local / remote-mod: rotating parts drawn by the client mod (two screenshots apart), airflow
#                       particles; the selftests pass on the (integrated) server.
#   remote-vanilla:     static models and the particles (resource pack), without errors.
# Screenshots: out/bulk/{bulk-a,bulk-b,bulk-fan}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

if (-not $vanilla) {
    $r = Invoke-SelfTest 'bulk.*'
    if ($r.Pass -lt 14) { throw "expected at least 14 bulk selftests to pass, got $($r.Pass)" }
}

Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1 ~'
Start-Sleep -Milliseconds 500
# millstone on a generator (left)
Invoke-Chat '/setblock ~5 ~ ~-3 levitate:kinetics_test_source ["levitate:axis"=1]'
Invoke-Chat '/setblock ~5 ~1 ~-3 levitate:millstone'
# crushing wheels (centre), driven from behind
Invoke-Chat '/setblock ~7 ~1 ~-1 levitate:kinetics_test_source ["levitate:axis"=0]'
Invoke-Chat '/setblock ~7 ~1 ~1 levitate:kinetics_test_source ["levitate:axis"=0]'
Invoke-Chat '/setblock ~6 ~1 ~-1 levitate:crushing_wheel ["levitate:axis"=0]'
Invoke-Chat '/setblock ~6 ~1 ~1 levitate:crushing_wheel ["levitate:axis"=0]'
# a fan blowing towards the camera (right)
Invoke-Chat '/setblock ~7 ~1 ~3 levitate:kinetics_test_source ["levitate:axis"=0]'
Invoke-Chat '/setblock ~6 ~1 ~3 levitate:encased_fan ["levitate:facing"=4]'
# a fan with a nozzle (far right)
Invoke-Chat '/setblock ~5 ~ ~5 levitate:kinetics_test_source ["levitate:axis"=1]'
Invoke-Chat '/setblock ~5 ~1 ~5 levitate:encased_fan ["levitate:facing"=1]'
Invoke-Chat '/setblock ~5 ~2 ~5 levitate:nozzle ["levitate:facing"=1]'
Start-Sleep -Seconds 2
$mark = Get-LogMark
Invoke-Chat '/create kinetics ~5 ~ ~-3 speed 64'
Invoke-Chat '/create kinetics ~7 ~1 ~-1 speed 64'
Invoke-Chat '/create kinetics ~7 ~1 ~1 speed -64'
Invoke-Chat '/create kinetics ~7 ~1 ~3 speed -128'
Invoke-Chat '/create kinetics ~5 ~ ~5 speed 128'
Wait-LogLine 'encased fan at .*: pushing' 30 -Since $mark | Out-Null
if (-not $vanilla) {
    Wait-LogLine 'kinetics client: drawing \d+ rotating parts' 30 | Out-Null
}
Start-Sleep -Seconds 1
Save-Screenshot 'bulk-a' | Out-Null
Start-Sleep -Milliseconds 400
Save-Screenshot 'bulk-b' | Out-Null
# look straight into the fan's current
Invoke-Chat '/tp @s ~ ~ ~3 facing ~6 ~1.5 ~3'
Start-Sleep -Seconds 1
Save-Screenshot 'bulk-fan' | Out-Null
Assert-NoErrors
