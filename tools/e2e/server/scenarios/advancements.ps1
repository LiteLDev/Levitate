# Advancements (U45) on BDS: awarding (toast to the client mod, silent entries, revoke), the
# inventory criteria and the root, an item applied to a log (casing), a placed block's owner and
# its kinetic trigger, and cogwheels meshing.
$r = Invoke-SelfTest 'advancements.*'
if ($r.Pass -lt 5) { throw "expected at least 5 advancements selftests to pass, got $($r.Pass)" }
Wait-LogLine 'advancements: .* made andesite_casing' 10 | Out-Null
Wait-LogLine 'advancements: .* made backtank' 10 | Out-Null
