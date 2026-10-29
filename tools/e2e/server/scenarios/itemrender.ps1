# World item renderer (U33) on BDS: the display item contract (sync to companions, providers,
# block changes) and the units that publish display items (depots, chutes, filter slots,
# placards, deployers), then the test pattern command.
$r = Invoke-SelfTest 'itemrender.*'
if ($r.Pass -lt 8) { throw "expected at least 8 itemrender selftests to pass, got $($r.Pass)" }

Send-Console 'create itemrender demo 0 -60 0'
Wait-LogLine 'itemrender: demo at 0 -60 0' 10 | Out-Null
Send-Console 'create itemrender clear'
Wait-LogLine 'itemrender: demo cleared' 10 | Out-Null
