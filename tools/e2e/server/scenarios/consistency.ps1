# Consistency (U35) on BDS: chute states read back as their values (0..n state lists), the chute's
# rotation handler, the single fluid registry and the shared Create strings.
$r = Invoke-SelfTest 'consistency.*'
if ($r.Fail -gt 0 -or $r.Pass -lt 4) { throw "expected 4 consistency selftests to pass, got $($r.Pass)" }
