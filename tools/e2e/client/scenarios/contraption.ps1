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
Start-Sleep -Milliseconds 900
$a = Save-Screenshot 'moving'
Start-Sleep -Milliseconds 900
$b = Save-Screenshot 'moving-later'
# the structure glides on the client: two pictures ~18 ticks apart differ around the screen centre
Add-Type -AssemblyName System.Drawing
$ia = [System.Drawing.Bitmap]::new($a); $ib = [System.Drawing.Bitmap]::new($b)
try {
    $changed = 0; $total = 0
    for ($x = [int]($ia.Width * 0.3); $x -lt [int]($ia.Width * 0.7); $x += 8) {
        for ($y = [int]($ia.Height * 0.32); $y -lt [int]($ia.Height * 0.85); $y += 8) { # below the chat lines
            $p = $ia.GetPixel($x, $y); $q = $ib.GetPixel($x, $y); $total++
            if ([Math]::Abs($p.R - $q.R) + [Math]::Abs($p.G - $q.G) + [Math]::Abs($p.B - $q.B) -gt 60) { $changed++ }
        }
    }
} finally { $ia.Dispose(); $ib.Dispose() }
Write-Step "moving contraption: $changed of $total sampled pixels changed between the two pictures"
if ($changed -lt [Math]::Max(10, $total * 0.01)) { throw "the moving contraption did not move on the client ($changed of $total pixels changed)" }
Start-Sleep -Milliseconds 2500

$mark = Get-LogMark
Invoke-Chat "/create contraption disassemble $id"
Wait-LogLine "contraption #$id disassembled: 5 placed, 0 dropped" 30 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1500
Save-Screenshot 'placed' | Out-Null

# the selftests on this world's server too (integrated or BDS host)
$r = Invoke-SelfTest 'contraption.*'
if ($r.Pass -lt 16) { throw "expected at least 16 contraption selftests to pass, got $($r.Pass)" }
