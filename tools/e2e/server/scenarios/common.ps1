# U3 common: block data, capabilities (items / fluids / heat), filters, recipes, screens (native
# through a test companion and the form fallback), value boxes and filter slots.
$r = Invoke-SelfTest 'common.*'
if ($r.Pass -lt 14) { throw "expected at least 14 common selftests to pass, got $($r.Pass)" }
Wait-LogLine 'screen common\.filter opened for .* \(native\)' 10 | Out-Null
Wait-LogLine 'screen common\.filter opened for .* \(form\)' 10 | Out-Null
Wait-LogLine 'filter: .* set slot 2 to minecraft:stone' 10 | Out-Null
Wait-LogLine 'value box lv_test at .* set to 32 RPM' 10 | Out-Null
Send-Console 'create highlight 0 -60 0'
Wait-LogLine 'highlight at 0 -60 0' 10 | Out-Null
