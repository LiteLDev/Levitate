# Basin unit (U14) on the client: a mixer over a basin of dirt and water on a seething blaze
# burner, a press over a basin of iron ingots, a kindled burner, lit (regular / soul) and empty
# burners, built by `/create basin demo` 4 blocks in front of the player.
#   local / remote-mod: the client mod draws the press head, the whisk, the blaze heads and the
#                       fluid in the basin moving (screenshots a second apart); the machines work.
#   remote-vanilla:     a client without the mod sees the static models, without errors, and (U54)
#                       a hopper filling a basin with ingots and copper, shown to it as client-only
#                       item entities on the basin's floor.
# Screenshots: out/basin/{basin-a,basin-b,basin-c}.png (+ basin-vanilla-items.png)
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
if ($vanilla) {
    # U54: a hopper fills a basin; vanilla clients see the stacks inside (VanillaItems.h)
    Invoke-Chat '/setblock -2 -59 4 levitate:basin'
    Invoke-Chat '/setblock -2 -58 4 hopper ["facing_direction"=0]'
    Invoke-Chat '/replaceitem block -2 -58 4 slot.container 0 iron_ingot 3'
    Invoke-Chat '/replaceitem block -2 -58 4 slot.container 1 copper_ingot 2'
    Start-Sleep -Seconds 3
    Invoke-Chat '/setblock -2 -58 4 air'
    Invoke-Chat '/tp @s -1.5 -56.5 2.5 facing -1.5 -59 4.5'
    Start-Sleep -Seconds 2
    Save-Screenshot 'basin-vanilla-items' | Out-Null
}
Assert-NoErrors
