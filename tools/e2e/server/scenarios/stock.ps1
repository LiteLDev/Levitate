# Stock network (U26): stock links, stock tickers, redstone requesters, factory gauges and table
# cloth shops. Every stock selftest (with a test packager provider: barrels stand in for U25's
# packagers), then the blocks placed from the console.
$mark = Get-LogMark
$r = Invoke-SelfTest 'stock.*'
if ($r.Pass -lt 11) { throw "expected at least 11 stock selftests to pass, got $($r.Pass)" }
Wait-LogLine 'stock: stock link placed at -?\d+ -?\d+ -?\d+ on network' 10 -Since $mark | Out-Null
Wait-LogLine 'stock: order of \d+ item\(s\) in \d+ stack\(s\) sent to .Home.' 10 -Since $mark | Out-Null
Wait-LogLine 'stock: redstone requester at -?\d+ -?\d+ -?\d+ sent its order to .Wall.' 10 -Since $mark | Out-Null
Wait-LogLine 'stock: factory gauge at -?\d+ -?\d+ -?\d+ restocks minecraft:coal x20' 10 -Since $mark | Out-Null
Wait-LogLine 'stock: factory gauge at -?\d+ -?\d+ -?\d+ panel 0 requested its ingredients' 10 -Since $mark | Out-Null

# The blocks exist and keep their states (placed from the console).
Send-Console 'tickingarea add 0 -64 0 15 -40 15 lv_stock_e2e true'
Wait-Seconds 3
Send-Console 'setblock 3 -60 3 levitate:stock_ticker ["levitate:facing"=2]'
Send-Console 'setblock 5 -60 3 levitate:redstone_requester ["levitate:axis"=1]'
Send-Console 'setblock 7 -60 3 levitate:stock_link ["levitate:face"=1,"levitate:facing"=3]'
Send-Console 'setblock 9 -60 3 levitate:factory_gauge ["levitate:face"=0,"levitate:facing"=2,"levitate:panels"=5]'
Wait-Seconds 1
$mark = Get-LogMark
Send-Console 'testforblock 9 -60 3 levitate:factory_gauge ["levitate:panels"=5]'
Wait-LogLine 'Successfully found the block' 10 -Since $mark | Out-Null
foreach ($x in 3, 5, 7, 9) { Send-Console "setblock $x -60 3 air" }
Send-Console 'tickingarea remove lv_stock_e2e'
