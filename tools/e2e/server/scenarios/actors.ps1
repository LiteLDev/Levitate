# Contraption actors (U22): every actors selftest (harvester, plough, roller fill / tunnel /
# settings, portable storage interfaces, contraption controls, seats), then a harvester carried
# by the contraption test anchor from the console with its log lines.
$r = Invoke-SelfTest 'actors.*'
if ($r.Pass -lt 12) { throw "expected at least 12 actors selftests to pass, got $($r.Pass)" }

# A harvester on a stone, driven over ripe wheat with `/create contraption`.
Send-Console 'tickingarea add 0 -64 0 15 -40 15 lv_actors_e2e true'
Wait-Seconds 3
Send-Console 'fill 2 -61 6 9 -58 6 air'
Send-Console 'fill 4 -61 6 7 -61 6 farmland'
Send-Console 'setblock 4 -60 6 wheat ["growth"=7]'
Send-Console 'setblock 5 -60 6 wheat ["growth"=7]'
# the harvester hangs on the block behind it (Create's attached actor rule), so no glue is needed
Send-Console 'setblock 3 -60 6 levitate:mechanical_harvester ["levitate:facing"=5]'
Send-Console 'setblock 2 -60 6 stone'
Wait-Seconds 1
$mark = Get-LogMark
Send-Console 'create contraption assemble 2 -60 6'
$line = Wait-LogLine 'contraption #(\d+) assembled: 2 blocks' 20 -Since $mark
$id = [regex]::Match($line, 'contraption #(\d+) assembled').Groups[1].Value
Send-Console "create contraption move $id 4 0 0 40"
Wait-LogLine 'actors: harvester harvested minecraft:wheat at 4 -60 6' 20 -Since $mark | Out-Null
Wait-LogLine 'actors: harvester harvested minecraft:wheat at 5 -60 6' 20 -Since $mark | Out-Null
Wait-Seconds 3
Send-Console "create contraption disassemble $id"
Wait-LogLine "contraption #$id disassembled: 2 placed, 0 dropped" 20 -Since $mark | Out-Null
Send-Console 'fill 2 -61 6 9 -58 6 air'
Send-Console 'tickingarea remove lv_actors_e2e'
