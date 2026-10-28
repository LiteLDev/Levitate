# Tracks (U5): track blocks, placement shapes, straight runs and curves placed by a simulated
# player with the track item, the track graph, path finding, edge points and persistence.
$mark = Get-LogMark
$r = Invoke-SelfTest 'tracks.*'
if ($r.Pass -lt 8) { throw "expected at least 8 tracks selftests to pass, got $($r.Pass)" }
Wait-LogLine 'tracks: \S+ selected the track at' 10 -Since $mark | Out-Null
Wait-LogLine 'tracks: \S+ connected .* with a curve of length' 10 -Since $mark | Out-Null
Wait-LogLine 'tracks: \S+ placed \d+ straight tracks' 10 -Since $mark | Out-Null
