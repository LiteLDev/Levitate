# Advancements (U45) on BDS: awarding (toast to the client mod, silent entries, revoke), the
# inventory criteria and the root, an item applied to a log (casing), a placed block's owner and
# its kinetic trigger, and cogwheels meshing. U49: the triggers wired in the units (press / spout
# counters, millstone, linked controller, stock ticker / table cloth / factory gauge, trains and
# the display link on a station).
$r = Invoke-SelfTest 'advancements.*'
if ($r.Pass -lt 10) { throw "expected at least 10 advancements selftests to pass, got $($r.Pass)" }
Wait-LogLine 'advancements: .* made andesite_casing' 10 | Out-Null
Wait-LogLine 'advancements: .* made backtank' 10 | Out-Null
Wait-LogLine 'advancements: .* made millstone' 10 | Out-Null
Wait-LogLine 'advancements: .* made linked_controller' 10 | Out-Null
Wait-LogLine 'advancements: .* made factory_gauge' 10 | Out-Null
Wait-LogLine 'advancements: .* made display_board_0' 10 | Out-Null
