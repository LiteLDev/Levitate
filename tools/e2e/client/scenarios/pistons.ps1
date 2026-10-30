# Linear contraptions (U20) on a real client.
#   local / remote-mod: a rope pulley lowers a platform (the client mod draws the rope and the
#                       magnet down to it), a piston pushes a block out; the selftests pass on the
#                       (integrated) server.
#   remote-vanilla:     the same blocks without the client mod: static models, no errors.
#   elevator contact:   right-click opens Create's native contact screen (type an identifier,
#                       confirm); remote-vanilla gets the form.
# Screenshots: out/pistons/{built,pulley-moving-1..4,pulley-placed,piston-extended,gantry-moving-1..5,gantry-placed,
#              contact-screen|contact-form}.png
$vanilla = $script:Mode -eq 'remote-vanilla'
# Sets a test generator's speed and waits for the server to confirm it (sent again when lost; a
# failed attempt is pictured).
function Set-GeneratorSpeed([string]$rel, [int]$rpm, [string]$picture) {
    try { Invoke-ChatConfirmed "/create kinetics $rel speed $rpm" "Generator at -?\d+ -?\d+ -?\d+ now turns at $rpm " | Out-Null }
    catch { Save-Screenshot $picture | Out-Null; throw }
}

# Two pictures of a moving contraption must differ in a region of the screen (by default right of
# the chat lines, above the hotbar): the structure moves on the client (U34).
function Assert-ContraptionMoved([string]$a, [string]$b, [string]$what, [double]$x0 = 0.42, [double]$x1 = 0.85, [double]$y0 = 0.05, [double]$y1 = 0.85) {
    Add-Type -AssemblyName System.Drawing
    $ia = [System.Drawing.Bitmap]::new($a); $ib = [System.Drawing.Bitmap]::new($b)
    try {
        $changed = 0; $total = 0
        for ($x = [int]($ia.Width * $x0); $x -lt [int]($ia.Width * $x1); $x += 8) {
            for ($y = [int]($ia.Height * $y0); $y -lt [int]($ia.Height * $y1); $y += 8) {
                $p = $ia.GetPixel($x, $y); $q = $ib.GetPixel($x, $y); $total++
                if ([Math]::Abs($p.R - $q.R) + [Math]::Abs($p.G - $q.G) + [Math]::Abs($p.B - $q.B) -gt 60) { $changed++ }
            }
        }
    } finally { $ia.Dispose(); $ib.Dispose() }
    Write-Step "${what}: $changed of $total sampled pixels changed between the two pictures"
    if ($changed -lt [Math]::Max(10, $total * 0.01)) { throw "$what did not move on the client ($changed of $total pixels changed)" }
}


Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~2 ~'
Start-Sleep -Milliseconds 500
# rope pulley 6 blocks up, its rope, magnet and a planks platform (without glue only the block
# under the magnet hangs on it, as in Create); generator beside the pulley
Invoke-Chat '/setblock ~6 ~6 ~ levitate:rope_pulley ["levitate:axis"=2]'
Invoke-Chat '/setblock ~6 ~5 ~ levitate:rope'
Invoke-Chat '/setblock ~6 ~4 ~ levitate:pulley_magnet'
Invoke-Chat '/fill ~5 ~3 ~-1 ~7 ~3 ~1 oak_planks'
# a piston facing the player's left with two poles, pushing a gold block
Invoke-Chat '/setblock ~5 ~1 ~-3 levitate:piston_extension_pole ["levitate:facing"=5]'
Invoke-Chat '/setblock ~4 ~1 ~-3 levitate:piston_extension_pole ["levitate:facing"=5]'
Invoke-Chat '/setblock ~6 ~1 ~-3 levitate:mechanical_piston ["levitate:facing"=5,"levitate:axis_first"=0]'
Invoke-Chat '/setblock ~7 ~1 ~-3 gold_block'
Start-Sleep -Seconds 1
Save-Screenshot 'built' | Out-Null

$mark = Get-LogMark
Invoke-Chat '/setblock ~6 ~6 ~1 levitate:kinetics_test_source ["levitate:axis"=2]'
# slowly at first, for pictures on the way down (the log is read with some delay)
Invoke-Chat '/create kinetics ~6 ~6 ~1 speed 16'
Wait-LogLine 'pulley at -?\d+ -?\d+ -?\d+ assembled: \d+ blocks' 90 -Since $mark | Out-Null
$shots = foreach ($i in 1..4) { Save-Screenshot "pulley-moving-$i"; Start-Sleep -Milliseconds 700 }
Assert-ContraptionMoved $shots[0] $shots[3] 'the lowered block'
Invoke-Chat '/create kinetics ~6 ~6 ~1 speed 96'
if (-not $vanilla) { Wait-LogLine 'pistons: rendering \d+ rope' 30 -Since $mark | Out-Null }
Wait-LogLine 'pulley at -?\d+ -?\d+ -?\d+ disassembled' 120 -Since $mark | Out-Null
Start-Sleep -Seconds 1
Save-Screenshot 'pulley-placed' | Out-Null

# east-facing pistons extend with negative speed (Create's movement modifier)
$mark = Get-LogMark
Invoke-Chat '/setblock ~6 ~1 ~-2 levitate:kinetics_test_source ["levitate:axis"=2]'
Start-Sleep -Seconds 2
Set-GeneratorSpeed '~6 ~1 ~-2' -128 'speed-128'
Wait-LogLine 'piston at -?\d+ -?\d+ -?\d+ disassembled at offset 2' 90 -Since $mark | Out-Null
Start-Sleep -Seconds 1
Save-Screenshot 'piston-extended' | Out-Null

# a gantry (U20): a shaft line along x in front of the player, a carriage with a plank on it;
# turning the shafts runs the carriage west towards the start of the line
Invoke-Chat '/tp @s ~ ~ ~ facing ~6 ~2 ~6'
foreach ($dx in 3..9) { Invoke-Chat "/setblock ~$dx ~1 ~6 levitate:gantry_shaft [""levitate:facing""=5]" }
Invoke-Chat '/setblock ~8 ~2 ~6 levitate:gantry_carriage ["levitate:facing"=1,"levitate:axis_first"=0]'
Invoke-Chat '/setblock ~8 ~3 ~6 oak_planks'
Start-Sleep -Seconds 1
$mark = Get-LogMark
Invoke-Chat '/setblock ~2 ~1 ~6 levitate:kinetics_test_source ["levitate:axis"=0]'
Invoke-Chat '/setblock ~10 ~1 ~6 levitate:kinetics_test_source ["levitate:axis"=0]'
# pictures while it runs (taken at once: the log is read with some delay)
$shots = foreach ($i in 1..5) { Save-Screenshot "gantry-moving-$i"; Start-Sleep -Milliseconds 800 }
# above the (turning) shafts only
Assert-ContraptionMoved $shots[0] $shots[4] 'the gantry carriage' 0.42 0.85 0.3 0.62
Wait-LogLine 'gantry carriage at -?\d+ -?\d+ -?\d+ assembled: \d+ blocks' 60 -Since $mark | Out-Null
Wait-LogLine 'gantry contraption #\d+ stops at' 90 -Since $mark | Out-Null
Start-Sleep -Seconds 1
Save-Screenshot 'gantry-placed' | Out-Null

# the elevator contact's settings (U40): Create's native screen for the client mod (type a floor
# identifier, confirm), the form for vanilla clients
Invoke-Chat '/setblock 40 -60 40 levitate:elevator_contact ["levitate:facing"=3]'
Invoke-Chat '/tp @s 40.5 -60 43.5 180 25'
Press-Key '9'
Start-Sleep -Milliseconds 1000
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
if ($vanilla) {
    Wait-LogLine 'screen pistons\.elevator_contact opened for .* \(form\)' 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 1500
    Save-Screenshot 'contact-form' | Out-Null
    Close-Screen
} else {
    Wait-LogLine 'screen pistons\.elevator_contact opened for .* \(native\)' 20 -Since $mark | Out-Null
    $line = Wait-LogLine 'screen opened: .* panel=' 20 -Since $mark
    if ($line -notmatch 'panel=([\d.]+),([\d.]+) inv=([\d.]+),([\d.]+) gui=([\d.]+)x([\d.]+)') { throw "no layout in '$line'" }
    $px = [double]$Matches[1]; $py = [double]$Matches[2]; $gw = [double]$Matches[5]; $gh = [double]$Matches[6]
    Start-Sleep -Milliseconds 800
    Click (($px + 33) / $gw) (($py + 37) / $gh) -Relative   # the floor identifier box
    Type-Text 'L4'
    Start-Sleep -Milliseconds 400
    Save-Screenshot 'contact-screen' | Out-Null
    Click (($px + 159) / $gw) (($py + 100) / $gh) -Relative # confirm
    Wait-LogLine "elevator contact at 40 -60 40 named 'L4'" 20 -Since $mark | Out-Null
}
Invoke-Chat '/setblock 40 -60 40 air'

if (-not $vanilla) {
    $r = Invoke-SelfTest 'pistons.*'
    if ($r.Pass -lt 14) { throw "expected at least 14 pistons selftests to pass, got $($r.Pass)" }
}
Assert-NoErrors
