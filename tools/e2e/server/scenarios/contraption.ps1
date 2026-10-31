# Contraption engine (U4): every contraption selftest (assembly rules, glue, chassis, sticker,
# motion, rotation, collision, carried players, storage, actors, data carriers, persistence),
# then the test anchor commands from the console with their log lines.
$r = Invoke-SelfTest 'contraption.*'
if ($r.Pass -lt 16) { throw "expected at least 16 contraption selftests to pass, got $($r.Pass)" }
# U53 (the audit fixes: movement settings, sticky neighbours, empty colliders, running bearings,
# actor rules, chassis glue sides, super glue, lever rotation, stabilized sub-contraptions)
$r = Invoke-SelfTest 'contraption.u53_*'
if ($r.Pass -lt 9 -or $r.Fail -gt 0) { throw "expected 9 contraption.u53_* selftests to pass, got $($r.Pass) pass / $($r.Fail) fail" }
# Motion on clients (U34): the transform sent to companions, display spawn height, respawn steps
# for vanilla viewers.
$r = Invoke-SelfTest 'contraptionmotion.*'
if ($r.Pass -lt 3) { throw "expected 3 contraptionmotion selftests to pass, got $($r.Pass)" }

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
# Create's GlueCommand: /create glue <from> <to>
$mark = Get-LogMark
Send-Console 'create glue 4 -58 4 5 -58 4'
Wait-LogLine 'super glue added by command: 4 -58 4 \.\. 5 -58 4' 20 -Since $mark | Out-Null
Send-Console 'create glue clear 100'
Send-Console 'setblock 4 -55 4 air'
Send-Console 'setblock 4 -57 4 air'
Send-Console 'tickingarea remove lv_contraption_e2e'
