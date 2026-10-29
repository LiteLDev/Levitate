# Schematics (U29): schematic and quill, schematic table uploads, written schematics, the
# schematicannon (save an area, print it elsewhere, compare) and crafting blueprints. Every
# schematics selftest, then their log lines.
$mark = Get-LogMark
$r = Invoke-SelfTest 'schematics.*'
if ($r.Pass -lt 10) { throw "expected at least 10 schematics selftests to pass, got $($r.Pass)" }
Wait-LogLine 'schematic and quill: .* saved lv_selftest_quill' 10 -Since $mark | Out-Null
Wait-LogLine 'schematicannon at -?\d+ -?\d+ -?\d+: finished printing lv_selftest_print_compare\.mcstructure \(13 blocks\)' 10 -Since $mark | Out-Null
Wait-LogLine 'schematics: new schematic uploaded: .*/lv_selftest_table_upload\.mcstructure' 10 -Since $mark | Out-Null
Wait-LogLine 'schematicannon at -?\d+ -?\d+ -?\d+: printed a material checklist' 10 -Since $mark | Out-Null
Wait-LogLine 'crafting blueprint placed at -?\d+ -?\d+ -?\d+ \(3x3\)' 10 -Since $mark | Out-Null
Wait-LogLine 'crafting blueprint at -?\d+ -?\d+ -?\d+: .* crafted 4 minecraft:stick' 10 -Since $mark | Out-Null

# From the console: blocks place with their states and the command lists nothing for the console
Send-Console 'tickingarea add 0 -64 0 15 -40 15 lv_schematics_e2e true'
Wait-Seconds 3
Send-Console 'setblock 3 -60 3 levitate:schematicannon'
Send-Console 'setblock 5 -60 3 levitate:schematic_table ["levitate:facing"=2]'
Wait-Seconds 1
Send-Console 'testforblock 3 -60 3 levitate:schematicannon'
Wait-LogLine 'Successfully found the block' 10 | Out-Null
Send-Console 'setblock 3 -60 3 air'
Send-Console 'setblock 5 -60 3 air'
Send-Console 'tickingarea remove lv_schematics_e2e'
