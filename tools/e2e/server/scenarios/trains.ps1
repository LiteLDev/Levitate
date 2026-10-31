# Trains (U27): stations on a track line, bogeys from train casing, assembly rules, a train
# driven by its schedule between two stations (there and back, reversing), manual controls,
# collisions, derailment, the station flag sync, the TrainsExt hooks for signals and disassembly at a station;
# U57: seated conductors, reversing speed, a drill stalling the train, the station's auto-schedule,
# a train through a portal track into the nether, relocation overlap and the station display sources.
$mark = Get-LogMark
$r = Invoke-SelfTest 'trains.*' -TimeoutSeconds 900
if ($r.Pass -lt 19) { throw "expected at least 19 trains selftests to pass, got $($r.Pass)" }
Wait-LogLine 'trains: station placed at' 10 -Since $mark | Out-Null
Wait-LogLine 'trains: bogey created at' 10 -Since $mark | Out-Null
Wait-LogLine "trains: assembled train #\d+ '.*' with 1 carriages" 10 -Since $mark | Out-Null
Wait-LogLine "trains: train #\d+ '.*' heading to station 'U27 Station B'" 10 -Since $mark | Out-Null
Wait-LogLine "trains: train #\d+ '.*' arrived at station 'U27 Station B'" 10 -Since $mark | Out-Null
Wait-LogLine "trains: train #\d+ '.*' heading to station 'U27 Station A' \(\d+ blocks, reversing\)" 10 -Since $mark | Out-Null
Wait-LogLine "trains: train #\d+ '.*' arrived at station 'U27 Station A'" 10 -Since $mark | Out-Null
Wait-LogLine "trains: disassembled train #\d+" 10 -Since $mark | Out-Null
Wait-LogLine 'trains: .* is controlling train' 10 -Since $mark | Out-Null
Wait-LogLine "trains: train #\d+ '.*' collided with train" 10 -Since $mark | Out-Null
Wait-LogLine "trains: train #\d+ '.*' derailed: the track beneath it is gone" 10 -Since $mark | Out-Null
Wait-LogLine "trains: train #\d+ '.*' stalled" 10 -Since $mark | Out-Null
Wait-LogLine "trains: train #\d+ '.*' goes on after a stall" 10 -Since $mark | Out-Null
Wait-LogLine "trains: station '.*' applied its auto-schedule to train #\d+" 10 -Since $mark | Out-Null
Wait-LogLine "trains: train #\d+ '.*' carriage #\d+ passes into dimension 1" 10 -Since $mark | Out-Null
Wait-LogLine "contraption #\d+ moved from dimension 0 to 1" 10 -Since $mark | Out-Null
