# Item logistics (U12) on BDS: every funnels selftest (funnels, filters and amounts, redstone,
# player placement / insertion, chutes, smart chutes, fans, vaults and comparators, creative
# crate, tunnels; U49: the item hatch), then a funnel built with /setblock and powered from the console.
$r = Invoke-SelfTest 'funnels.*'
if ($r.Pass -lt 14) { throw "expected at least 14 funnels selftests to pass, got $($r.Pass)" }
Wait-LogLine 'funnels: .* deposited the held item through the item hatch at ' 10 | Out-Null
Wait-LogLine 'funnels: levitate:andesite_funnel placed at .* by ' 10 | Out-Null
Wait-LogLine 'funnels: item vault at .* formed \(2x2x3\)' 10 | Out-Null

Send-Console 'tickingarea add 0 -64 0 15 0 15 lv_funnels'
Wait-Seconds 3
$mark = Get-LogMark
Send-Console 'setblock 6 -58 6 chest'
Send-Console 'setblock 7 -58 6 levitate:brass_funnel ["levitate:facing"=5,"levitate:extracting"=1]'
Wait-Seconds 1
Send-Console 'setblock 7 -58 7 redstone_block'
Wait-LogLine 'funnels: funnel at 0:7:-58:6 powered' 15 -Since $mark | Out-Null
Send-Console 'setblock 7 -58 7 air'
Wait-LogLine 'funnels: funnel at 0:7:-58:6 unpowered' 15 -Since $mark | Out-Null
# a vault built with /fill forms one 2x2x3 structure
$mark = Get-LogMark
Send-Console 'fill 2 -58 9 3 -57 11 levitate:item_vault ["levitate:axis"=2]'
Wait-LogLine 'funnels: item vault at 2 -58 9 formed \(2x2x3\)' 15 -Since $mark | Out-Null
Send-Console 'fill 2 -58 9 3 -57 11 air'
Send-Console 'setblock 7 -58 6 air'
Send-Console 'setblock 6 -58 6 air'
Send-Console 'tickingarea remove lv_funnels'
