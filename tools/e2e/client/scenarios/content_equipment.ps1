# U49 equipment content on a real client: peculiar / haunted bells, cardboard armour, the item hatch.
#   local / remote-mod: the bells (the client mod draws the bell swinging), a peculiar bell turning
#                       haunted over soul fire, a haunted bell rung (soul particles on the dark spots),
#                       the full cardboard set worn and hiding as a box while sneaking (the vignette),
#                       an item hatch on a chest taking the held item (the hatch opens).
#   remote-vanilla:     the same without the client mod (bells drawn still, no vignette).
# Screenshots: out/content_equipment/{bells,bell-swing,bell-rung,soul-pulse,cardboard-armor,cardboard-hiding,cardboard-trim,
#              cardboard-hiding-first-person,hatch,hatch-open,hatch-closed}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

function Hold-Key([int]$vk, [scriptblock]$body) {
    Assert-GameForeground
    [BsE2E.Native]::Key([uint16]$vk, $false)
    try { & $body } finally { [BsE2E.Native]::Key([uint16]$vk, $true) }
    Start-Sleep -Milliseconds 300
}

Reset-CameraPerspective
Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set midnight'
Invoke-Chat '/clear @s'
Invoke-Chat '/tp @s ~ ~ ~ -90 25'      # look east (+x), down at the bells
Start-Sleep -Milliseconds 600
Invoke-Chat '/fill ~1 ~-1 ~-6 ~10 ~5 ~6 air'
Invoke-Chat '/fill ~1 ~-1 ~-6 ~10 ~-1 ~6 stone'
Start-Sleep -Milliseconds 400

# --- bells ---------------------------------------------------------------------------------------------------
# a haunted bell on the floor in front (facing east: rung from its west side), a peculiar bell beside
# it, a peculiar bell on a wall and one hanging over soul fire (it turns haunted)
$mark = Get-LogMark
Invoke-Chat '/setblock ~3 ~ ~ levitate:haunted_bell ["levitate:facing"=5,"levitate:attachment"=0]'
Invoke-Chat '/setblock ~3 ~ ~-2 levitate:peculiar_bell ["levitate:facing"=2,"levitate:attachment"=0]'
Invoke-Chat '/setblock ~5 ~ ~2 stone'
Invoke-Chat '/setblock ~4 ~ ~2 levitate:peculiar_bell ["levitate:facing"=5,"levitate:attachment"=2]'
Invoke-Chat '/setblock ~5 ~-1 ~-4 soul_soil'
Invoke-Chat '/setblock ~5 ~2 ~-4 stone'
Invoke-Chat '/setblock ~5 ~1 ~-4 levitate:peculiar_bell ["levitate:facing"=5,"levitate:attachment"=1]'
Invoke-Chat '/setblock ~5 ~ ~-4 soul_fire'
Wait-LogLine 'equipment: peculiar bell at .* became a haunted bell' 15 -Since $mark | Out-Null
Start-Sleep -Seconds 1
Save-Screenshot 'bells' | Out-Null

# a redstone block next to the peculiar bell (facing north) rings it: it swings north - south, across
# the view (the client mod draws the swing; vanilla clients see it still)
$mark = Get-LogMark
Invoke-Chat '/setblock ~3 ~ ~-3 redstone_block'
Wait-LogLine 'equipment: levitate:peculiar_bell at .* rang' 15 -Since $mark | Out-Null
Start-Sleep -Milliseconds 200
Save-Screenshot 'bell-swing' | Out-Null
if (-not $vanilla) { Wait-LogLine 'equipment client: bell at .* swings' 10 -Since $mark | Out-Null }

# ring the haunted bell from its west side (the crosshair is on its body)
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'equipment: levitate:haunted_bell at .* rang' 15 -Since $mark | Out-Null
Start-Sleep -Milliseconds 250
Save-Screenshot 'bell-rung' | Out-Null
Start-Sleep -Milliseconds 1500
Save-Screenshot 'soul-pulse' | Out-Null
try { Wait-LogLine 'advancements: .* made haunted_bell' 10 -Since $mark | Out-Null } catch { Write-Step 'haunted_bell advancement: no new award (earned before?)' }
Invoke-Chat '/time set noon'
Invoke-Chat '/fill ~1 ~ ~-6 ~10 ~5 ~6 air'
Start-Sleep -Milliseconds 400

# --- cardboard armour ----------------------------------------------------------------------------------------
Invoke-Chat '/replaceitem entity @s slot.armor.head 0 levitate:cardboard_helmet'
Invoke-Chat '/replaceitem entity @s slot.armor.chest 0 levitate:cardboard_chestplate'
Invoke-Chat '/replaceitem entity @s slot.armor.legs 0 levitate:cardboard_leggings'
Invoke-Chat '/replaceitem entity @s slot.armor.feet 0 levitate:cardboard_boots'
Invoke-Chat '/tp @s ~ ~ ~ -90 10'
Start-Sleep -Milliseconds 600
Press-Key F5; Press-Key F5          # third person, from the front
Start-Sleep -Milliseconds 900
Save-Screenshot 'cardboard-armor' | Out-Null
$mark = Get-LogMark
Hold-Key 0x10 {
    Wait-LogLine 'equipment: .* hides as a cardboard box' 15 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 900
    Save-Screenshot 'cardboard-hiding' | Out-Null
    Press-Key F5                   # back to first person (the box is not drawn there)
    Start-Sleep -Milliseconds 1200
    Save-Screenshot 'cardboard-hiding-first-person' | Out-Null
}
Wait-LogLine 'equipment: .* stops hiding' 15 -Since $mark | Out-Null
# a trimmed chestplate (the smithing table's item data, put on by a selftest helper): does the
# attachable draw the trim?
$mark = Get-LogMark
Invoke-Chat '/create selftest "equipment.cardboard_trim_showcase"'
Wait-LogLine 'SELFTEST (PASS|FAIL) equipment.cardboard_trim_showcase' 30 -Since $mark | Out-Null
Press-Key F5; Press-Key F5          # third person, from the front
Start-Sleep -Milliseconds 1200
Save-Screenshot 'cardboard-trim' | Out-Null
Press-Key F5
Start-Sleep -Milliseconds 400
Invoke-Chat '/clear @s'
Start-Sleep -Milliseconds 400

# --- item hatch ----------------------------------------------------------------------------------------------
# a chest with a hatch on its west side, facing it; the player holds apples and clicks the hatch
Invoke-Chat '/tp @s ~ ~ ~ -90 20'
Invoke-Chat '/setblock ~4 ~ ~ chest'
Invoke-Chat '/setblock ~3 ~ ~ levitate:item_hatch ["levitate:facing"=5]'
Invoke-Chat '/give @s apple 32'
Start-Sleep -Milliseconds 800
Save-Screenshot 'hatch' | Out-Null
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'funnels: .* deposited the held item through the item hatch at ' 15 -Since $mark | Out-Null
Start-Sleep -Milliseconds 150
Save-Screenshot 'hatch-open' | Out-Null
Start-Sleep -Milliseconds 1200
Save-Screenshot 'hatch-closed' | Out-Null

if (-not $vanilla) {
    $r = Invoke-SelfTest 'equipment.bell*'
    if ($r.Fail -gt 0) { throw "bell selftests failed: $($r.Fail)" }
}
Invoke-Chat '/clear @s'
Invoke-Chat '/fill ~1 ~-1 ~-6 ~10 ~5 ~6 air'
Assert-NoErrors
