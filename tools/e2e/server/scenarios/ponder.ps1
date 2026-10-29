# U30 Ponder on BDS: the server side (`/create ponder` resolves ids, sends `ponder.open` to
# companions only, refuses vanilla clients and the console) and the shipped scene data.
$r = Invoke-SelfTest 'ponder.*'
if ($r.Pass -lt 3) { throw "expected 3 ponder selftests to pass, got $($r.Pass)" }
$mark = Get-LogMark
Send-Console 'create ponder shaft'
Wait-LogLine 'Only players can ponder' 10 -Since $mark | Out-Null