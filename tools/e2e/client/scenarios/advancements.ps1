# Advancements (U45) on a real client, plus the worldshaper preview (R179).
#   local / remote-mod: a granted advancement shows Create's toast; /create advancements opens the
#                       native tree; a configured worldshaper outlines its brush.
#   remote-vanilla:     the game's toast and the advancement form.
# Screenshots: out/advancements/{toast,tree,tree-scrolled,form,shaper-preview}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

Reset-CameraPerspective
Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/create advancements revoke belt'
Start-Sleep -Milliseconds 500

# a noisy advancement: toast + chat line
$mark = Get-LogMark
Invoke-Chat '/create advancements grant belt'
Wait-LogLine 'advancements: .* made belt' 20 -Since $mark | Out-Null
if (-not $vanilla) { Wait-LogLine 'advancements: toast belt' 20 -Since $mark | Out-Null }
Start-Sleep -Milliseconds 900
Save-Screenshot 'toast' | Out-Null
Start-Sleep -Seconds 5

# the tree (vanilla: a form)
$mark = Get-LogMark
Invoke-Chat '/create advancements'
Wait-LogLine 'advancements: .* opened the advancement tree' 20 -Since $mark | Out-Null
if (-not $vanilla) { Wait-LogLine 'advancements: tree screen opened' 20 -Since $mark | Out-Null }
Start-Sleep -Milliseconds 900
if ($vanilla) {
    Save-Screenshot 'form' | Out-Null
    Close-Screen
} else {
    Move-Mouse 0.5 0.5 -Relative
    Start-Sleep -Milliseconds 300
    Save-Screenshot 'tree' | Out-Null
    Scroll -3
    Start-Sleep -Milliseconds 500
    Save-Screenshot 'tree-scrolled' | Out-Null
    Press-Key E
    Start-Sleep -Milliseconds 800
}

# the worldshaper preview: a configured 3x3x3 cuboid aimed at the ground
if (-not $vanilla) {
    Invoke-Chat '/tp @s ~ ~ ~ -90 45'
    Invoke-Chat '/fill ~1 ~-1 ~-4 ~8 ~-1 ~4 stone'
    Invoke-Chat '/clear @s'
    Invoke-Chat '/replaceitem entity @s slot.hotbar 0 levitate:handheld_worldshaper'
    Press-Key '1'
    Start-Sleep -Milliseconds 600
    # sneak + right-click: the screen; width / height / length 3; confirm
    Assert-GameForeground
    [BsE2E.Native]::Key([uint16]0x10, $false)
    try { Click 0.5 0.5 -Relative -Right; Start-Sleep -Milliseconds 900 } finally { [BsE2E.Native]::Key([uint16]0x10, $true) }
    Start-Sleep -Milliseconds 400
    foreach ($y in 0.396, 0.449, 0.503) { Move-Mouse 0.5 $y -Relative; Start-Sleep -Milliseconds 200; Scroll 2; Start-Sleep -Milliseconds 300 }
    Save-Screenshot 'shaper-screen' | Out-Null
    $mark = Get-LogMark
    Click 0.670 0.710 -Relative
    Wait-LogLine 'equipment: .* configured the worldshaper \(Cuboid\)' 20 -Since $mark | Out-Null
    Start-Sleep -Seconds 1
    Save-Screenshot 'shaper-preview' | Out-Null
}
Invoke-Chat '/create advancements revoke belt'
