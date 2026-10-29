# Redstone (U23): every redstone selftest (signals in and out through the game's circuit,
# links and the link API, analog lever, pulse repeater / extender / timer, latches, smart
# observer, threshold switch and its screen, contacts, placement, persistence), then a link
# pair driven from the console with its log lines.
$r = Invoke-SelfTest 'redstone.*'
if ($r.Pass -lt 18) { throw "expected at least 18 redstone selftests to pass, got $($r.Pass)" }

# A transmitter powered by a redstone block and a receiver lighting a lamp, from the console.
Send-Console 'tickingarea add 0 -64 0 15 -40 15 lv_redstone_e2e true'
Wait-Seconds 3
$mark = Get-LogMark
Send-Console 'setblock 4 -60 4 levitate:redstone_link ["levitate:facing"=1]'
Send-Console 'setblock 10 -60 4 levitate:redstone_link ["levitate:facing"=1,"levitate:receiver"=1]'
Send-Console 'setblock 11 -60 4 redstone_lamp'
Wait-Seconds 1
Send-Console 'setblock 3 -60 4 redstone_block'
Wait-Seconds 2
Send-Console 'testforblock 11 -60 4 lit_redstone_lamp'
Wait-LogLine 'Successfully found the block' 10 -Since $mark | Out-Null
Send-Console 'setblock 3 -60 4 air'
Send-Console 'setblock 4 -60 4 air'
Send-Console 'setblock 10 -60 4 air'
Send-Console 'setblock 11 -60 4 air'
Send-Console 'tickingarea remove lv_redstone_e2e'
