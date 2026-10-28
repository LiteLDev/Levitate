# Contraption engine (U4) on a real client: a glued structure is built in front of the player,
# assembled with the test anchor, turned and moved, then placed back.
# Screenshots: out/contraption/{built,turning,turned,moving,placed}.png
#   built    - the display entities right after assembly
#   turning  - mid rotation (client mod: blocks turned by the exact angle; vanilla: axis-aligned)
#   turned   - after a quarter turn
#   moving   - while rising
#   placed   - after disassembly (real blocks again)
Reset-CameraPerspective
Start-Sleep -Milliseconds 500
$mark = Get-LogMark
Invoke-Chat '/create contraption demo'
$line = Wait-LogLine 'contraption #(\d+) assembled: 5 blocks' 30 -Since $mark
$id = [regex]::Match($line, 'contraption #(\d+) assembled').Groups[1].Value
Start-Sleep -Milliseconds 1500
Save-Screenshot 'built' | Out-Null

Invoke-Chat "/create contraption rotate $id y 90 80"
Start-Sleep -Milliseconds 1800
Save-Screenshot 'turning' | Out-Null
Start-Sleep -Milliseconds 3500
Save-Screenshot 'turned' | Out-Null

Invoke-Chat "/create contraption move $id 0 2 0 60"
Start-Sleep -Milliseconds 1500
Save-Screenshot 'moving' | Out-Null
Start-Sleep -Milliseconds 3000

$mark = Get-LogMark
Invoke-Chat "/create contraption disassemble $id"
Wait-LogLine "contraption #$id disassembled: 5 placed, 0 dropped" 30 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1500
Save-Screenshot 'placed' | Out-Null

# the selftests on this world's server too (integrated or BDS host)
$r = Invoke-SelfTest 'contraption.*'
if ($r.Pass -lt 16) { throw "expected at least 16 contraption selftests to pass, got $($r.Pass)" }
