# U25 packages: every packages selftest (package items, packagers, re-packager, chain conveyors,
# frogports, postboxes), then a packager driven from the console with its log lines.
$r = Invoke-SelfTest 'packages.*'
if ($r.Pass -lt 11) { throw "expected at least 11 packages selftests to pass, got $($r.Pass)" }

# A packager boxing a chest from the console: chest, packager facing east, a redstone pulse.
Send-Console 'tickingarea add 0 -64 0 15 -40 15 lv_packages_e2e true'
Wait-Seconds 3
Send-Console 'setblock 4 -60 4 chest'
Send-Console 'setblock 5 -60 4 levitate:packager ["levitate:facing"=5]'
Wait-Seconds 1
Send-Console 'replaceitem block 4 -60 4 slot.container 0 cobblestone 32'
Wait-Seconds 1
$mark = Get-LogMark
Send-Console 'setblock 5 -59 4 redstone_block'
Wait-LogLine 'packages: packager at 0:5:-60:4 activated by redstone' 20 -Since $mark | Out-Null
Wait-LogLine "packages: packager at 0:5:-60:4 packed a package for '' \(1 stacks\)" 20 -Since $mark | Out-Null
Send-Console 'setblock 5 -59 4 air'
Send-Console 'setblock 5 -60 4 air'
Send-Console 'setblock 4 -60 4 air'
Send-Console 'kill @e[type=item]'
Send-Console 'tickingarea remove lv_packages_e2e'
