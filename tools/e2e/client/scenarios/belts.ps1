# Belts (U11) on the client.
#   local / remote-mod: the selftests pass on the (integrated) server; the client mod draws the belt
#                       itself (scrolling band, turning pulleys; two screenshots apart) once it
#                       has the belt speeds.
#   remote-vanilla:     a client without the mod sees the static belt, depot and ejector models.
#   U41: encased sloped and vertical belts, a pulling belt funnel on the belt, and the belt
#        connector preview (dust from a selected shaft towards the shaft looked at; the client mod
#        draws it itself, vanilla clients get it from the server).
#   U55: the weighted ejector target outline and arc preview of the client mod.
# Screenshots: out/belts/{belts-a,belts-b,belts-casing,connector-preview,ejector-arc}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

if (-not $vanilla) {
    $r = Invoke-SelfTest 'belts.*'
    if ($r.Pass -lt 26) { throw "expected at least 26 belts selftests to pass, got $($r.Pass)" }
}

Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/tp @s ~ ~ ~ facing ~10 ~-3 ~3'
Start-Sleep -Milliseconds 500
# a 5 block belt along +x two blocks to the right, driven from its start; a depot at its end;
# a dyed sloped piece and an ejector next to it
Invoke-Chat '/setblock ~3 ~ ~1 levitate:kinetics_test_source ["levitate:axis"=2]'
Invoke-Chat '/setblock ~3 ~ ~2 levitate:belt ["levitate:facing"=3,"levitate:part"=0]'
Invoke-Chat '/fill ~4 ~ ~2 ~6 ~ ~2 levitate:belt ["levitate:facing"=3,"levitate:part"=1]'
Invoke-Chat '/setblock ~7 ~ ~2 levitate:belt ["levitate:facing"=3,"levitate:part"=2]'
Invoke-Chat '/setblock ~8 ~ ~2 levitate:depot'
Invoke-Chat '/setblock ~3 ~ ~4 levitate:belt ["levitate:facing"=3,"levitate:slope"=1,"levitate:part"=0,"levitate:dyed"=1,"levitate:color"=14]'
Invoke-Chat '/setblock ~4 ~1 ~4 levitate:belt ["levitate:facing"=3,"levitate:slope"=1,"levitate:part"=2,"levitate:dyed"=1,"levitate:color"=14]'
Invoke-Chat '/setblock ~6 ~ ~4 levitate:weighted_ejector ["levitate:facing"=3]'
Start-Sleep -Seconds 2
Invoke-Chat '/create kinetics ~3 ~ ~1 speed -64'
if (-not $vanilla) {
    Wait-LogLine 'belts client: drawing \d+ belt segments' 30 | Out-Null
}
Start-Sleep -Seconds 1
Save-Screenshot 'belts-a' | Out-Null
Start-Sleep -Milliseconds 300
Save-Screenshot 'belts-b' | Out-Null

# U41: a brass-encased slope and an andesite-encased vertical belt; a pulling belt funnel with its
# chest on the north side of the main belt
Invoke-Chat '/setblock ~3 ~ ~6 levitate:belt ["levitate:facing"=3,"levitate:slope"=1,"levitate:part"=0,"levitate:casing"=2]'
Invoke-Chat '/setblock ~4 ~1 ~6 levitate:belt ["levitate:facing"=3,"levitate:slope"=1,"levitate:part"=2,"levitate:casing"=2]'
Invoke-Chat '/setblock ~7 ~ ~6 levitate:belt ["levitate:facing"=2,"levitate:slope"=3,"levitate:part"=0,"levitate:casing"=1]'
Invoke-Chat '/setblock ~7 ~1 ~6 levitate:belt ["levitate:facing"=2,"levitate:slope"=3,"levitate:part"=2,"levitate:casing"=1]'
Invoke-Chat '/setblock ~5 ~1 ~3 chest'
Invoke-Chat '/setblock ~5 ~1 ~2 levitate:andesite_funnel ["levitate:facing"=2,"levitate:shape"=4]'
Start-Sleep -Seconds 2
Invoke-Chat '/tp @s ~ ~ ~ -40 10'      # towards the encased slope and vertical belt (+x, +z)
Start-Sleep -Milliseconds 800
Save-Screenshot 'belts-casing' | Out-Null

# the connector preview: two shafts (axis z) 3 blocks apart within reach north of the player,
# select the left one, then look at the right one
Invoke-Chat '/fill ~-2 ~ ~-3 ~4 ~2 ~-1 air'
Invoke-Chat '/setblock ~-1 ~1 ~-2 levitate:shaft ["levitate:axis"=2]'
Invoke-Chat '/setblock ~2 ~1 ~-2 levitate:shaft ["levitate:axis"=2]'
Invoke-Chat '/replaceitem entity @s slot.weapon.mainhand 0 levitate:belt_connector 4'
Start-Sleep -Milliseconds 800
Invoke-Chat '/tp @s ~ ~ ~ 153 3'      # at the left shaft (1 west, 2 north)
Start-Sleep -Milliseconds 600
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'belts: .* selected the shaft at ' 15 -Since $mark | Out-Null
if (-not $vanilla) { Wait-LogLine 'belts client: connector preview from ' 15 -Since $mark | Out-Null }
Invoke-Chat '/tp @s ~ ~ ~ -135 3'     # at the right shaft (2 east, 2 north)
if (-not $vanilla) { Wait-LogLine 'belts client: connector preview towards .* \(3 blocks, valid\)' 15 -Since $mark | Out-Null }
Start-Sleep -Milliseconds 1500
Save-Screenshot 'connector-preview' | Out-Null
Invoke-Chat '/replaceitem entity @s slot.weapon.mainhand 0 air'

# U55: the weighted ejector's target (Create's EjectorTargetHandler): sneak + right-click a block
# with the ejector selects it; the client mod outlines it and draws the arc from where the ejector
# would be placed (the ground one block east, in line with the target)
if (-not $vanilla) {
    Invoke-Chat '/fill 1 -60 -1 7 -58 1 air'
    Invoke-Chat '/setblock 5 -60 0 stone'
    Invoke-Chat '/replaceitem entity @s slot.weapon.mainhand 0 levitate:weighted_ejector 4'
    Invoke-Chat '/tp @s 0.5 -60 0.5 -90 12.6'   # at the stone (5 -60 0)
    Start-Sleep -Milliseconds 800
    $mark = Get-LogMark
    [BsE2E.Native]::Key([uint16]0x10, $false)   # sneak
    Start-Sleep -Milliseconds 300
    Click 0.5 0.5 -Relative -Right
    Start-Sleep -Milliseconds 300
    [BsE2E.Native]::Key([uint16]0x10, $true)
    Wait-LogLine 'belts: .* selected the ejector target ' 15 -Since $mark | Out-Null
    Wait-LogLine 'belts client: ejector target ' 15 -Since $mark | Out-Null
    Invoke-Chat '/tp @s 0.5 -60 0.5 -90 58.3'   # at the ground east of the player (the ejector would go to 1 -60 0)
    Wait-LogLine 'belts client: ejector arc to .* \(valid\)' 15 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 1200
    Save-Screenshot 'ejector-arc' | Out-Null
    Invoke-Chat '/replaceitem entity @s slot.weapon.mainhand 0 air'
}
Assert-NoErrors