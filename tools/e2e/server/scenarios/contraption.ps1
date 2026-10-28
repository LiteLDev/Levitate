# Contraption engine (U4): every contraption selftest (assembly rules, glue, chassis, sticker,
# motion, rotation, collision, carried players, storage, actors, data carriers, persistence),
# then the test anchor commands from the console with their log lines.
$r = Invoke-SelfTest 'contraption.*'
if ($r.Pass -lt 16) { throw "expected at least 16 contraption selftests to pass, got $($r.Pass)" }

# The test anchor through the console: a single stone near the world origin.
Send-Console 'tickingarea add 0 -64 0 15 -40 15 lv_contraption_e2e true'
Wait-Seconds 3
Send-Console 'setblock 4 -58 4 stone'
Send-Console 'setblock 4 -57 4 gold_block'
Wait-Seconds 1
$mark = Get-LogMark
Send-Console 'create contraption assemble 4 -58 4'
$line = Wait-LogLine 'contraption #(\d+) assembled: 1 blocks' 20 -Since $mark
$id = [regex]::Match($line, 'contraption #(\d+) assembled').Groups[1].Value
Send-Console "create contraption move $id 0 3 0 20"
Wait-Seconds 2
Send-Console "create contraption rotate $id y 90 10"
Wait-Seconds 2
Send-Console 'create contraption list'
Send-Console "create contraption disassemble $id"
Wait-LogLine "contraption #$id disassembled: 1 placed, 0 dropped" 20 -Since $mark | Out-Null
Send-Console 'setblock 4 -55 4 air'
Send-Console 'setblock 4 -57 4 air'
Send-Console 'tickingarea remove lv_contraption_e2e'
