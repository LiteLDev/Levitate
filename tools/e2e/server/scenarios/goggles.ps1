# Goggles (U8): the wrench (rotation, dismantling, pick-up, minecarts), the goggle overlay
# (kinetic stats, overstress hint, providers, the query channel) and `/create overlay reset`.
$mark = Get-LogMark
$r = Invoke-SelfTest 'goggles.*'
if ($r.Pass -lt 9) { throw "expected at least 9 goggles selftests to pass, got $($r.Pass)" }
Wait-LogLine 'wrench: \S+ rotated levitate:kinetics_test_shaft' 10 -Since $mark | Out-Null
Wait-LogLine 'wrench: \S+ rotated levitate:sticker' 10 -Since $mark | Out-Null
Wait-LogLine 'wrench: \S+ dismantled levitate:kinetics_test_shaft' 10 -Since $mark | Out-Null
Wait-LogLine 'wrench: \S+ dismantled minecraft:hopper' 10 -Since $mark | Out-Null
Wait-LogLine 'wrench: \S+ broke a minecraft:minecart' 10 -Since $mark | Out-Null
Wait-LogLine 'goggles: overlay for \S+ at .* lines, client mod\)' 10 -Since $mark | Out-Null

$mark = Get-LogMark
Send-Console 'create overlay reset'
Wait-LogLine 'goggles: overlay reset for console' 10 -Since $mark | Out-Null
