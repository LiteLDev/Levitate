# Shared interaction and redstone dispatch (U36): the order of the use-on handlers (a belt's own
# wrench handling before U8's generic wrench), placement / next-tick dispatch, and comparators
# reading a fluid tank, an item vault and a depot (also through a solid block) via the shared
# analog registry.
$r = Invoke-SelfTest 'hooks.*'
if ($r.Pass -lt 5 -or $r.Fail -gt 0) { throw "expected the 5 hooks selftests to pass, got pass=$($r.Pass) fail=$($r.Fail)" }
# the generic wrench turned the shaft in hooks.wrench_order (one log line per action)
Wait-LogLine 'wrench: .* rotated levitate:shaft at' 10 | Out-Null
