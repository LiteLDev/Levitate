# Wave-1 integration (U32): kinetics and tracks sync over the companion protocol, kinetic records
# in BlockData (and the migration of the wave-1 keys), BlockData carried by contraptions, the
# chassis value box, the shared interaction dispatch and the display entities' crash safety.
$r = Invoke-SelfTest 'integration.*'
if ($r.Pass -lt 8 -or $r.Fail -gt 0) { throw "expected the 8 integration selftests to pass, got pass=$($r.Pass) fail=$($r.Fail)" }
Wait-LogLine 'kinetics: moved \d+ block records from \d+ chunks into block data' 10 | Out-Null
Wait-LogLine 'value box range at .* set to 5 by' 10 | Out-Null
Wait-LogLine 'value box range at .* opened for .* \(native\)' 10 | Out-Null
