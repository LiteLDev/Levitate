# Signals (U28): signal and observer blocks, binding with the item (simulated player), signal
# sections, simulated trains through the SignalsExt contract (occupancy, reservations, cross
# signal chains), redstone input, comparator output, observer output and filters.
$mark = Get-LogMark
$r = Invoke-SelfTest 'signals.*'
if ($r.Pass -lt 7) { throw "expected at least 7 signals selftests to pass, got $($r.Pass)" }
Wait-LogLine 'signals: \S+ selected the track at' 10 -Since $mark | Out-Null
Wait-LogLine 'signals: signal at \S+ bound to signal:' 10 -Since $mark | Out-Null
Wait-LogLine 'signals: signal at \S+ switched to cross_signal' 10 -Since $mark | Out-Null
Wait-LogLine 'signals: track observer at \S+ powered' 10 -Since $mark | Out-Null
Wait-LogLine 'signals: signal at \S+ forced red by redstone' 10 -Since $mark | Out-Null
