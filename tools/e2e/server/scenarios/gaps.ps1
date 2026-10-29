# U37 gaps on BDS: a sequenced gearshift turns a bearing by exactly 90 degrees (also as two equal
# steps) and moves a piston exactly 3 blocks; a sequence limit holds a gantry after 2 blocks; the
# client mod's train channels (trains.interact keys / stop, trains.throttle throttle / horn) drive a
# train; then the text field actions and the kinetic feedback of this unit.
$mark = Get-LogMark
$r = Invoke-SelfTest 'gaps.*' -TimeoutSeconds 600
if ($r.Fail -gt 0) { throw "gaps selftests failed: $($r.Fail)" }
if ($r.Pass -lt 9) { throw "expected the 9 gaps selftests to pass, got $($r.Pass)" }
Wait-LogLine 'transmission: sequenced gearshift at .* runs instruction 1 \(turn_angle\)' 10 -Since $mark | Out-Null
Wait-LogLine 'transmission: sequenced gearshift at .* runs instruction 1 \(turn_distance\)' 10 -Since $mark | Out-Null
Wait-LogLine 'trains: .* is controlling train' 10 -Since $mark | Out-Null
Wait-LogLine "trains: train #\d+ '.*' throttle 50%" 10 -Since $mark | Out-Null
Wait-LogLine "trains: train #\d+ '.*' honks" 10 -Since $mark | Out-Null
Wait-LogLine 'trains: .* stopped controlling train' 10 -Since $mark | Out-Null
