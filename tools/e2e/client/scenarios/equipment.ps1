# Equipment (U31) on a real client.
#   local / remote-mod: the toolbox screen (native), the toolbox radial menu (hold Left Alt), the
#                       worldshaper and wand of symmetry screens, a potato cannon shot, worn
#                       diving gear; the selftests pass on the (integrated) server.
#   remote-vanilla:     the same items without the client mod: forms instead of screens.
# Screenshots: out/equipment/{blocks,toolbox-screen,toolbox-filled,radial,radial-compartments,worldshaper-screen,
#              symmetry-screen,cannon-shot,diving-gear}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

# native screens close with the inventory key (the framework pops its host scene itself)
function Close-Native { if ($vanilla) { Close-Screen } else { Press-Key E; Start-Sleep -Milliseconds 800 } }

function Hold-Key([int]$vk, [scriptblock]$body) {
    Assert-GameForeground
    [BsE2E.Native]::Key([uint16]$vk, $false)
    try { & $body } finally { [BsE2E.Native]::Key([uint16]$vk, $true) }
    Start-Sleep -Milliseconds 300
}

Reset-CameraPerspective
Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/tp @s ~ ~ ~ -90 20'      # look east (+x), slightly down
Start-Sleep -Milliseconds 600
Invoke-Chat '/fill ~2 ~-1 ~-4 ~8 ~5 ~4 air'
Invoke-Chat '/fill ~2 ~-1 ~-4 ~8 ~-1 ~4 stone'
Start-Sleep -Milliseconds 400

# the blocks: toolboxes in a few colours, both backtanks, a lectern controller
Invoke-Chat '/setblock ~3 ~ ~ levitate:brown_toolbox ["levitate:facing"=3]'
Invoke-Chat '/setblock ~4 ~ ~-2 levitate:red_toolbox ["levitate:facing"=3]'
Invoke-Chat '/setblock ~4 ~ ~2 levitate:cyan_toolbox ["levitate:facing"=3]'
Invoke-Chat '/setblock ~6 ~ ~-3 levitate:copper_backtank ["levitate:facing"=3]'
Invoke-Chat '/setblock ~6 ~ ~3 levitate:netherite_backtank ["levitate:facing"=3]'
Invoke-Chat '/setblock ~7 ~ ~ levitate:lectern_controller ["levitate:facing"=3]'
Start-Sleep -Seconds 1
Save-Screenshot 'blocks' | Out-Null

# right-click the brown toolbox: the native screen (vanilla: a form)
Invoke-Chat '/give @s torch 64'
Invoke-Chat '/give @s cobblestone 64'
Start-Sleep -Milliseconds 400
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'equipment: toolbox at .* opened by ' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 900
Save-Screenshot 'toolbox-screen' | Out-Null
if (-not $vanilla) {
    # put the torches and the cobblestone in (click their hotbar slots in the screen's inventory)
    Click 0.367 0.792 -Relative
    Start-Sleep -Milliseconds 400
    Click 0.402 0.792 -Relative
    Start-Sleep -Milliseconds 600
    Save-Screenshot 'toolbox-filled' | Out-Null
}
Close-Native
Start-Sleep -Milliseconds 500
Save-Screenshot 'toolbox-closed' | Out-Null

# the lid and drawers (Create's ToolboxRenderer): held open from the console as if a player had the
# screen open; the client mod animates them, vanilla clients see the open model. The red toolbox
# faces the player, the brown one shows its back.
Invoke-Chat '/setblock ~4 ~ ~-2 levitate:red_toolbox ["levitate:facing"=1]'
Invoke-Chat '/tp @s ~ ~ ~ -110 25'
$mark = Get-LogMark
Invoke-Chat '/create toolbox lid ~4 ~ ~-2 600'
Invoke-Chat '/create toolbox lid ~3 ~ ~ 600'
Wait-LogLine 'equipment: toolbox lid at .* opened' 15 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1500
Save-Screenshot 'toolbox-lid-open' | Out-Null
$mark = Get-LogMark
Invoke-Chat '/create toolbox lid ~4 ~ ~-2 0'
Wait-LogLine 'equipment: toolbox lid at .* closed' 15 -Since $mark | Out-Null
Save-Screenshot 'toolbox-lid-closing' | Out-Null
Invoke-Chat '/create toolbox lid ~3 ~ ~ 0'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'toolbox-lid-shut' | Out-Null
Invoke-Chat '/tp @s ~ ~ ~ -90 20'
Start-Sleep -Milliseconds 400

if (-not $vanilla) {
    # hold Left Alt: the radial menu of the nearby toolboxes; release over the first entry
    $mark = Get-LogMark
    Hold-Key 0x12 {
        Wait-LogLine 'equipment: toolbox radial menu opened' 15 -Since $mark | Out-Null
        Start-Sleep -Milliseconds 500
        # point at the top entry (62 GUI px above the centre): the nearest toolbox
        $gui = Wait-LogLine 'screen opened: .* gui=' 10 -Since $mark
        $gh = if ($gui -match 'gui=[\d.]+x([\d.]+)') { [double]$Matches[1] } else { 300 }
        $script:radialTop = 0.5 - 62 / $gh
        Move-Mouse 0.5 $script:radialTop -Relative
        Start-Sleep -Milliseconds 500
        Save-Screenshot 'radial' | Out-Null
    }
    # three toolboxes are in range (SELECT_BOX): the release picked the nearest one (the brown
    # toolbox, top entry) and the menu now shows its compartments; click the top one (the torches)
    Start-Sleep -Milliseconds 500
    Save-Screenshot 'radial-compartments' | Out-Null
    Click 0.5 $script:radialTop -Relative
    Wait-LogLine 'equipment: .* equipped minecraft:torch from the toolbox' 15 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 600
    Save-Screenshot 'radial-equipped' | Out-Null

    # dyeing in a crafting table keeps the contents (Create's ToolboxDyeingRecipe): pick the filled
    # brown toolbox up (left-click), craft it with a red dye, place the red toolbox and open it
    Invoke-Chat '/tp @s ~ ~ ~ -90 20'
    Start-Sleep -Milliseconds 500
    $mark = Get-LogMark
    Click 0.5 0.5 -Relative
    Wait-LogLine 'equipment: toolbox at .* picked up by ' 15 -Since $mark | Out-Null
    Invoke-Chat '/clear @s torch'
    Invoke-Chat '/clear @s levitate:handheld_worldshaper'
    Invoke-Chat '/give @s red_dye'
    Invoke-Chat '/setblock ~3 ~ ~ crafting_table'
    Start-Sleep -Milliseconds 600
    Click 0.5 0.5 -Relative -Right
    Start-Sleep -Milliseconds 1500
    Save-Screenshot 'crafting-table' | Out-Null
    # hotbar: red dye (slot 1), the filled brown toolbox (slot 2) -> the grid's middle row
    # (hover each slot before clicking it: the screen only takes a click on the slot under the mouse)
    function Click-Slot([double]$x, [double]$y) { Move-Mouse $x $y -Relative; Start-Sleep -Milliseconds 300; Click $x $y -Relative; Start-Sleep -Milliseconds 400 }
    Click-Slot 0.540 0.729; Click-Slot 0.5805 0.365
    Click-Slot 0.5055 0.729; Click-Slot 0.547 0.365
    Start-Sleep -Milliseconds 400
    Save-Screenshot 'crafting-grid' | Out-Null
    # take the result and put it into the third hotbar slot
    $mark = Get-LogMark
    Click-Slot 0.730 0.365
    Wait-LogLine 'equipment: .* dyed a toolbox into levitate:red_toolbox in the crafting grid \(contents kept\)' 15 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 400
    Click-Slot 0.574 0.729
    Save-Screenshot 'crafted' | Out-Null
    Close-Screen
    # place the crafted toolbox on the floor and open it: it has the brown toolbox's contents
    Invoke-Chat '/setblock ~3 ~ ~ air'
    Press-Key '3'
    Start-Sleep -Milliseconds 400
    $mark = Get-LogMark
    Click 0.5 0.5 -Relative -Right
    Wait-LogLine 'equipment: toolbox placed at .* by .* \([1-9]\d* items\)' 15 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 600
    Click 0.5 0.5 -Relative -Right
    Wait-LogLine 'equipment: toolbox at .* opened by ' 15 -Since $mark | Out-Null
    Start-Sleep -Milliseconds 900
    Save-Screenshot 'crafted-toolbox-screen' | Out-Null
    Close-Native
    Invoke-Chat '/fill ~3 ~ ~-1 ~6 ~ ~1 air'
} else {
    Invoke-Chat '/create toolbox'
    Start-Sleep -Milliseconds 900
    Save-Screenshot 'toolbox-form' | Out-Null
    Close-Screen
}

# worldshaper: sneak + right-click opens its screen
Invoke-Chat '/clear @s'
Invoke-Chat '/replaceitem entity @s slot.hotbar 1 levitate:handheld_worldshaper'
Press-Key '2'                      # slot 0 is linked to the toolbox (torches)
Start-Sleep -Milliseconds 500
Hold-Key 0x10 { Click 0.5 0.5 -Relative -Right; Start-Sleep -Milliseconds 900 }
Save-Screenshot 'worldshaper-screen' | Out-Null
Close-Native

# wand of symmetry: right-click a block places the mirror, sneak + right-click opens the screen
Invoke-Chat '/clear @s'
Invoke-Chat '/replaceitem entity @s slot.hotbar 1 levitate:wand_of_symmetry'
Press-Key '2'                      # slot 0 is linked to the toolbox (torches)
Start-Sleep -Milliseconds 500
$mark = Get-LogMark
Invoke-Chat '/tp @s ~ ~ ~ -90 60'
Start-Sleep -Milliseconds 500
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'equipment: .* placed a symmetry mirror at ' 15 -Since $mark | Out-Null
Hold-Key 0x10 { Click 0.5 0.5 -Relative -Right; Start-Sleep -Milliseconds 900 }
Save-Screenshot 'symmetry-screen' | Out-Null
Close-Native

# potato cannon: a shot at the ground
Invoke-Chat '/clear @s'
Invoke-Chat '/replaceitem entity @s slot.hotbar 1 levitate:potato_cannon'
Invoke-Chat '/give @s potato 16'
Press-Key '2'                      # slot 0 is linked to the toolbox (torches)
Invoke-Chat '/gamemode survival'
Start-Sleep -Milliseconds 600
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'equipment: .* fired a potato cannon \(potato\)' 15 -Since $mark | Out-Null
Start-Sleep -Milliseconds 120
Save-Screenshot 'cannon-shot' | Out-Null
Invoke-Chat '/gamemode creative'

# worn diving gear, seen from the front
Invoke-Chat '/clear @s'
Invoke-Chat '/replaceitem entity @s slot.armor.head 0 levitate:copper_diving_helmet'
Invoke-Chat '/replaceitem entity @s slot.armor.chest 0 levitate:copper_backtank'
Invoke-Chat '/replaceitem entity @s slot.armor.feet 0 levitate:copper_diving_boots'
Start-Sleep -Milliseconds 600
Press-Key F5; Press-Key F5
Start-Sleep -Milliseconds 900
Save-Screenshot 'diving-gear' | Out-Null
Reset-CameraPerspective

if (-not $vanilla) {
    $r = Invoke-SelfTest 'equipment.*'
    if ($r.Pass -lt 16) { throw "expected at least 16 equipment selftests to pass, got $($r.Pass)" }
}
Invoke-Chat '/clear @s'
Invoke-Chat '/fill ~2 ~-1 ~-4 ~8 ~5 ~4 air'
Assert-NoErrors
