# Fluid transport (U17) on BDS: the selftests on a real world (connections, placement by a player,
# wrench / casing / brackets, pumps moving fluid tank to tank, open ends, valves, smart pipe filters,
# fluid collisions, milk effects, persistence), then a pipe line built with /setblock and inspected
# with the debug command.
$mark = Get-LogMark
$r = Invoke-SelfTest 'pipes.*'
if ($r.Pass -lt 13) { throw "expected at least 13 pipes selftests to pass, got $($r.Pass)" }
Wait-LogLine 'pipes: \S+ wrenched .*: made a glass pipe' 10 -Since $mark | Out-Null
Wait-LogLine 'pipes: pump at .* now runs at 64 RPM' 10 -Since $mark | Out-Null
Wait-LogLine 'pipes: valve at .* opened' 10 -Since $mark | Out-Null
Wait-LogLine 'pipes: minecraft:water and minecraft:lava met in the pipe' 10 -Since $mark | Out-Null

# Two pipes next to the spawn: the second one makes the first turn towards it.
Send-Console 'tickingarea add 0 -64 0 15 0 15 lv_pipes'
Wait-Seconds 3
$mark = Get-LogMark
Send-Console 'setblock 2 -58 4 levitate:fluid_pipe'
Send-Console 'setblock 3 -58 4 levitate:fluid_pipe'
Wait-Seconds 1
Send-Console 'create pipes 2 -58 4'
Wait-LogLine 'pipes: levitate:fluid_pipe at 2 -58 4' 15 -Since $mark | Out-Null
Send-Console 'setblock 2 -58 4 air'
Send-Console 'setblock 3 -58 4 air'
Send-Console 'tickingarea remove lv_pipes'
