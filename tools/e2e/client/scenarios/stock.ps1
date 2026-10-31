# Stock network (U26) on a real client.
#   local / remote-mod: the blocks' models, the redstone requester screen and the stock keeper
#                       request screen (a pig sitting on a seat beside a stock ticker is its keeper;
#                       U56: Create only counts riders of seats)
#                       as native screens; the selftests pass on the (integrated) server.
#   remote-vanilla:     the same right-clicks open the form fallbacks.
# Screenshots: out/stock/{built,requester,keeper}.png
$vanilla = $script:Mode -eq 'remote-vanilla'
$kind    = if ($vanilla) { 'form' } else { 'native' }
if (-not $vanilla) { Wait-LogLine 'companion joined: ' 90 | Out-Null }
# native screens close with the inventory key (the framework's own close), forms with Esc
function Close-StockScreen { if ($vanilla) { Close-Screen } else { Press-Key E; Start-Sleep -Milliseconds 800 } }

Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/clear @s'
Invoke-Chat '/tp @s ~ ~ ~ facing ~3 ~1 ~'
Start-Sleep -Milliseconds 500
# the blocks: a wall of redstone requesters ahead, a ticker, a link and a gauge to the side
Invoke-Chat '/fill ~3 ~ ~-1 ~3 ~2 ~1 levitate:redstone_requester ["levitate:axis"=0]'
Invoke-Chat '/setblock ~4 ~ ~-3 levitate:stock_ticker ["levitate:facing"=1]'
Invoke-Chat '/setblock ~4 ~ ~-4 levitate:stock_link ["levitate:face"=0,"levitate:facing"=1]'
Invoke-Chat '/setblock ~4 ~ ~3 levitate:factory_gauge ["levitate:face"=0,"levitate:facing"=1,"levitate:panels"=15]'
Invoke-Chat '/setblock ~4 ~1 ~3 levitate:factory_gauge ["levitate:face"=1,"levitate:facing"=1,"levitate:panels"=5]'
Start-Sleep -Seconds 1
Save-Screenshot 'built' | Out-Null
Invoke-Chat '/tp @s ~ ~ ~ facing ~4 ~-0.5 ~3'
Start-Sleep -Milliseconds 800
Save-Screenshot 'gauges' | Out-Null
Invoke-Chat '/tp @s ~ ~ ~ facing ~3 ~1 ~'
Start-Sleep -Milliseconds 500

$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine "screen stock\.requester opened for .* \($kind\)" 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1200
Save-Screenshot 'requester' | Out-Null
Close-StockScreen

# a stock ticker with a keeper: a pig sitting on a seat beside it (mobs on a free seat sit down)
Invoke-Chat '/fill ~3 ~ ~-1 ~3 ~2 ~1 air'
Invoke-Chat '/setblock ~3 ~ ~ levitate:stock_ticker ["levitate:facing"=1]'
Invoke-Chat '/setblock ~3 ~ ~1 levitate:white_seat'
Invoke-Chat '/summon pig ~3 ~ ~1'
Start-Sleep -Milliseconds 500
Start-Sleep -Milliseconds 1000   # the pig sits down on the seat
Start-Sleep -Milliseconds 800
Invoke-Chat '/tp @s ~ ~ ~ facing ~3 ~-0.9 ~1'   # the eyes are 1.62 above the feet: aim at the seated pig
Start-Sleep -Milliseconds 600
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine "screen stock\.request opened for .* \($kind\)" 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 1200
Save-Screenshot 'keeper' | Out-Null
Close-StockScreen
Invoke-Chat '/kill @e[type=pig]'
Invoke-Chat '/setblock ~3 ~ ~1 air'

if ($script:Mode -eq 'local') {
    $r = Invoke-SelfTest 'stock.*'
    if ($r.Pass -lt 11) { throw "expected at least 11 stock selftests to pass, got $($r.Pass)" }
}
Assert-NoErrors
