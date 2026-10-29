# Kinetic feedback (U37) on BDS: rotation indicators of generators changing their speed level (only
# for players wearing goggles, 2 ticks later, Create's colours / speeds) and the ambient kinetic /
# cog soundscapes (which blocks play, range, pitch, volume).
$mark = Get-LogMark
$r = Invoke-SelfTest 'gaps.feedback_*'
if ($r.Pass -lt 2) { throw "expected 2 kinetic feedback selftests to pass, got $($r.Pass)" }
Wait-LogLine 'kinetics: rotation indicators at .* \(16 RPM\) for 1 players' 10 -Since $mark | Out-Null
Wait-LogLine 'kinetics: rotation indicators at .* \(-64 RPM\) for 1 players' 10 -Since $mark | Out-Null
