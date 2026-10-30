# Redstone (U23): every redstone selftest (signals in and out through the game's circuit,
# links and the link API, analog lever, pulse repeater / extender / timer, latches, smart
# observer, threshold switch and its screen, contacts, placement, persistence, U49: desk bell and rose
# quartz lamp), then a link pair driven from the console with its log lines and two rose quartz lamps.
$r = Invoke-SelfTest 'redstone.*'
if ($r.Pass -lt 20) { throw "expected at least 20 redstone selftests to pass, got $($r.Pass)" }

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
# U49: two rose quartz lamps; a redstone block lights the second and the first stays dark
$mark = Get-LogMark
Send-Console 'setblock 4 -60 8 levitate:rose_quartz_lamp'
Send-Console 'setblock 5 -60 8 levitate:rose_quartz_lamp'
Wait-Seconds 1
Send-Console 'setblock 6 -60 8 redstone_block'
Wait-LogLine 'rose quartz lamp at 5 -60 8 activated \(1 lamps in its cluster\)' 10 -Since $mark | Out-Null
Wait-Seconds 1
Send-Console 'testforblock 5 -60 8 levitate:rose_quartz_lamp ["levitate:powering"=1,"levitate:powered"=1,"levitate:activate"=0]'
Wait-LogLine 'Successfully found the block' 10 -Since $mark | Out-Null
Send-Console 'setblock 4 -60 8 air'
Send-Console 'setblock 5 -60 8 air'
Send-Console 'setblock 6 -60 8 air'
Send-Console 'tickingarea remove lv_redstone_e2e'
