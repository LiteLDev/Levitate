# Kinetic feedback (U37) on a real client: wearing goggles, a generator that changes its speed
# level shows Create's rotation indicator ring (green slow, blue medium, pink fast) around its axis;
# a running cogwheel plays the cog soundscape (checked by the selftests; sounds can't be screenshot).
# Works the same for vanilla clients (the particle and the sounds come from the resource pack).
# Screenshots: out/gaps_feedback/{indicator-slow,indicator-medium,indicator-fast,no-goggles}.png
$vanilla = $script:Mode -eq 'remote-vanilla'
if (-not $vanilla) {
    Wait-LogLine 'companion: server accepted the handshake' 90 | Out-Null
}

Invoke-Chat '/gamemode creative'
Invoke-Chat '/clear @s'
Invoke-Chat '/time set noon'
# a generator along x with a cogwheel, looked at from 3 blocks away (ground at y -61)
Invoke-Chat '/tp @s 0.5 -60 3.5'
Start-Sleep -Milliseconds 500
Invoke-Chat '/setblock 4 -59 3 levitate:kinetics_test_cog ["levitate:axis"=0]'
Invoke-Chat '/tp @s 0.5 -60 3.5 facing 3.5 -59 3.5'
Invoke-Chat '/replaceitem entity @s slot.armor.head 0 levitate:goggles'
Start-Sleep -Seconds 1

function Show-Indicator([string]$command, [string]$rpm, [string]$shot) {
    $mark = Get-LogMark
    Invoke-Chat $command
    Start-Sleep -Milliseconds 250
    Save-Screenshot $shot | Out-Null
    Start-Sleep -Milliseconds 250
    Save-Screenshot "$shot-later" | Out-Null
    Wait-LogLine "kinetics: rotation indicators at 3 -59 3 \($rpm RPM\) for 1 players" 20 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 1500
}

Show-Indicator '/setblock 3 -59 3 levitate:kinetics_test_source ["levitate:axis"=0]' '16' 'indicator-slow'
Show-Indicator '/create kinetics 3 -59 3 speed -64' '-64' 'indicator-medium'
Show-Indicator '/create kinetics 3 -59 3 speed 128' '128' 'indicator-fast'

# without goggles: no burst reaches the player
Invoke-Chat '/replaceitem entity @s slot.armor.head 0 air'
Start-Sleep -Milliseconds 500
$mark = Get-LogMark
Invoke-Chat '/create kinetics 3 -59 3 speed 16'
Start-Sleep -Milliseconds 350
Save-Screenshot 'no-goggles' | Out-Null
Start-Sleep -Seconds 1
if (Get-LeviLog -Since $mark | Select-String -Pattern 'kinetics: rotation indicators at 3 -59 3 \(16 RPM\)') { throw 'indicators sent to a player without goggles' }

if ($script:Mode -eq 'local') {
    $r = Invoke-SelfTest 'gaps.feedback_*'
    if ($r.Pass -lt 2) { throw "expected 2 kinetic feedback selftests to pass, got $($r.Pass)" }
}
Invoke-Chat '/setblock 3 -59 3 air'
Invoke-Chat '/setblock 4 -59 3 air'
Assert-NoErrors
