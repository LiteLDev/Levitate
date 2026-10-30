# Belts (U11) on BDS: the selftests on a real world (connecting, rules, transport, queueing,
# belt-to-belt transfer, entities, breaking, editing, depots, processing, ejectors, persistence;
# U41: tunnels, belt funnels and chutes on real belts, sloped collision, explosions, ejector
# redstone, the connector preview), then a belt built with commands that carries a stack into a
# depot and gets a belt funnel, and an ejector powered from the console.
$r = Invoke-SelfTest 'belts.*'
if ($r.Pass -lt 26) { throw "expected at least 26 belts selftests to pass, got $($r.Pass)" }
Wait-LogLine 'belts: \d+ belt segment\(s\) at .* blown up' 10 | Out-Null
Wait-LogLine 'funnels: belt funnel at .* set to extended by ' 10 | Out-Null
Wait-LogLine 'belts: lv_test_\d+ connected a belt from .* \(5 blocks\)' 10 | Out-Null
Wait-LogLine 'belts: belt broken at .* \(5 blocks\)' 10 | Out-Null
Wait-LogLine 'belts: ejector at .* launched minecraft:redstone x4' 10 | Out-Null

# A belt laid with /setblock (start, middle, end; facing east) gets its metadata and turns.
Send-Console 'tickingarea add 0 -64 0 15 0 15 lv_belts'
Wait-Seconds 3
$mark = Get-LogMark
# a funnel and its chest wait where the belt will be laid: the funnel becomes a pulling belt funnel
Send-Console 'setblock 3 -57 3 chest'
Send-Console 'setblock 3 -57 2 levitate:andesite_funnel ["levitate:facing"=2]'
Wait-Seconds 1
Send-Console 'setblock 2 -58 1 levitate:kinetics_test_source ["levitate:axis"=2]'
Send-Console 'setblock 2 -58 2 levitate:belt ["levitate:facing"=3,"levitate:part"=0]'
Send-Console 'setblock 3 -58 2 levitate:belt ["levitate:facing"=3,"levitate:part"=1]'
Send-Console 'setblock 4 -58 2 levitate:belt ["levitate:facing"=3,"levitate:part"=2]'
Wait-Seconds 1
Send-Console 'create kinetics 4 -58 2'
Wait-LogLine 'kinetics: Speed: 16 RPM' 15 -Since $mark | Out-Null
Wait-LogLine 'funnels: funnel at 0:3:-57:2 is now on a belt \(shape 4\)' 15 -Since $mark | Out-Null
# an ejector powered by a redstone block (the shared redstone watch)
Send-Console 'setblock 8 -58 8 levitate:weighted_ejector ["levitate:facing"=3]'
Wait-Seconds 1
Send-Console 'setblock 8 -58 9 redstone_block'
Wait-LogLine 'belts: ejector at 8 -58 8 powered' 15 -Since $mark | Out-Null
Send-Console 'setblock 8 -58 9 air'
Wait-LogLine 'belts: ejector at 8 -58 8 unpowered' 15 -Since $mark | Out-Null
Send-Console 'setblock 8 -58 8 air'
Send-Console 'setblock 3 -57 2 air'
Send-Console 'setblock 3 -57 3 air'
Send-Console 'tickingarea remove lv_belts'