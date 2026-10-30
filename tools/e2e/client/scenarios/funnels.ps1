# Item logistics (U12) on a real client: the blocks of the unit built in front of the player
# (models, connected vault textures), then items falling through a chute (display items, drawn
# for every client) and a funnel flapping while it extracts.
# With the client mod (local / remote-mod) horizontal funnels are drawn every frame and their flap
# strips swing after each transfer (Create's FunnelRenderer, U41).
# Screenshots: out/funnels/{blocks,vault,chute-moving,funnel-flap,funnel-flap-b}.png
Reset-CameraPerspective
Invoke-Chat '/gamemode creative'
Invoke-Chat '/tp @s ~ ~ ~ -90 15'      # look east (+x), slightly down
Start-Sleep -Milliseconds 800
Invoke-Chat '/fill ~2 ~-1 ~-4 ~9 ~6 ~4 air'
Invoke-Chat '/fill ~2 ~-1 ~-4 ~9 ~-1 ~4 stone'
Start-Sleep -Milliseconds 500

# a row of blocks: funnels on a chest (wall: extracting, floor: inserting), tunnels, chutes, smart chute, crate
Invoke-Chat '/setblock ~4 ~ ~-3 chest'
Invoke-Chat '/setblock ~3 ~ ~-3 levitate:andesite_funnel ["levitate:facing"=4,"levitate:extracting"=1]'
Invoke-Chat '/setblock ~4 ~1 ~-3 levitate:brass_funnel ["levitate:facing"=1]'
Invoke-Chat '/setblock ~4 ~ ~-1 levitate:andesite_tunnel'
Invoke-Chat '/setblock ~4 ~ ~0 levitate:brass_tunnel ["levitate:axis"=2]'
Invoke-Chat '/setblock ~4 ~ ~1 levitate:smart_chute ["levitate:powered"=0]'
Invoke-Chat '/setblock ~4 ~ ~3 levitate:creative_crate'
# a 2x2x3 vault behind them
Invoke-Chat '/fill ~7 ~ ~-1 ~8 ~1 ~1 levitate:item_vault ["levitate:axis"=2]'
Start-Sleep -Milliseconds 1500
Save-Screenshot 'blocks' | Out-Null
if ($script:Mode -ne 'remote-vanilla') { Wait-LogLine 'funnels client: drawing \d+ funnels' 30 | Out-Null }
Wait-LogLine 'funnels: item vault at .* formed \(2x2x3\)' 20 | Out-Null
Save-Screenshot 'vault' | Out-Null

# a chute column: a chest on top, two chutes, a chest below; fill the top chest
Invoke-Chat '/setblock ~5 ~ ~2 chest'
Invoke-Chat '/setblock ~5 ~1 ~2 levitate:chute'
Invoke-Chat '/setblock ~5 ~2 ~2 levitate:chute ["levitate:shape"=1]'
Invoke-Chat '/setblock ~5 ~3 ~2 chest'
Start-Sleep -Milliseconds 800
Invoke-Chat '/replaceitem block ~5 ~3 ~2 slot.container 0 cobblestone 64'
Invoke-Chat '/replaceitem block ~5 ~3 ~2 slot.container 1 diamond 64'
Start-Sleep -Milliseconds 300
Save-Screenshot 'chute-moving' | Out-Null

# the extracting funnel on the first chest flaps when it drops an item
Invoke-Chat '/replaceitem block ~4 ~ ~-3 slot.container 0 iron_ingot 64'
Start-Sleep -Milliseconds 250
Save-Screenshot 'funnel-flap' | Out-Null
Start-Sleep -Milliseconds 300
Save-Screenshot 'funnel-flap-b' | Out-Null

if ($script:Mode -eq 'local') {
    $r = Invoke-SelfTest 'funnels.*'
    if ($r.Pass -lt 13) { throw "expected at least 13 funnels selftests to pass, got $($r.Pass)" }
    $r = Invoke-SelfTest 'belts.link_*'
    if ($r.Pass -lt 10) { throw "expected the 10 belt / logistics selftests to pass, got $($r.Pass)" }
}
Invoke-Chat '/fill ~2 ~-1 ~-4 ~9 ~6 ~4 air'
