# Basin unit (U14) on the client: a mixer over a basin of dirt and water on a seething blaze
# burner, a press over a basin of iron ingots, a kindled burner, lit (regular / soul) and empty
# burners, built by `/create basin demo` 4 blocks in front of the player.
#   local / remote-mod: the client mod draws the press head, the whisk, the blaze heads and the
#                       fluid in the basin moving (screenshots a second apart); the machines work.
#   remote-vanilla:     a client without the mod sees the static models, without errors.
# Screenshots: out/basin/{basin-a,basin-b,basin-c}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

if (-not $vanilla) {
    $r = Invoke-SelfTest 'basin.*'
    if ($r.Pass -lt 15) { throw "expected at least 15 basin selftests to pass, got $($r.Pass)" }
}

Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~1.2 ~'
Start-Sleep -Milliseconds 500
$mark = Get-LogMark
Invoke-Chat '/create basin demo'
Wait-LogLine 'basin demo built at' 30 -Since $mark | Out-Null
# step back so the whole row is in view
Invoke-Chat '/tp @s ~-3 ~1 ~ facing ~10 ~0 ~'
Start-Sleep -Seconds 2
if (-not $vanilla) {
    Wait-LogLine 'basin client: drawing \d+ moving parts' 30 | Out-Null
}
Save-Screenshot 'basin-a' | Out-Null
Wait-LogLine 'press at .* compacted' 60 -Since $mark | Out-Null
Save-Screenshot 'basin-b' | Out-Null
Wait-LogLine 'mixer at .* mixed' 60 -Since $mark | Out-Null
Start-Sleep -Milliseconds 700
Save-Screenshot 'basin-c' | Out-Null
Assert-NoErrors
