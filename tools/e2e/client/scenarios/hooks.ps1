# Shared interaction and redstone dispatch (U36) on a real client: every click below goes through
# common/Interactions.h (no unit hooks GameMode itself any more).
#   - a casing, then the wrench on a belt: the belt's own wrench handling (remove the casing) runs
#     before U8's generic wrench;
#   - a right-click on a deployer's front face hands it the held stack;
#   - chains on a chain conveyor select it;
#   - an empty hand turns a valve handle;
#   - a right-click on a seat sits down;
#   - a water bucket into a fluid tank: a comparator reading the tank lights a lamp (shared
#     comparator output registry).
#   local / remote-mod: plus the hooks.* selftests on the (integrated) server.
#   remote-vanilla:     the same clicks from a client without the mod (server-side dispatch).
# Screenshots: out/hooks/{belt-encased,belt-wrenched,deployer,chain,valve,seat,tank-lamp}.png
$vanilla = $script:Mode -eq 'remote-vanilla'

if (-not $vanilla) {
    $r = Invoke-SelfTest 'hooks.*'
    if ($r.Pass -lt 5 -or $r.Fail -gt 0) { throw "expected the 5 hooks selftests to pass, got pass=$($r.Pass) fail=$($r.Fail)" }
}

Reset-CameraPerspective
Invoke-Chat '/gamemode creative'
Invoke-Chat '/time set noon'
Invoke-Chat '/tp @s 0.5 -60 0.5'
Start-Sleep -Milliseconds 500
Invoke-Chat '/fill -2 -60 -6 14 -55 8 air'
Invoke-Chat '/clear @s'
# hotbar: 1 wrench, 2 andesite casing, 3 cobblestone, 4 chains, 5 water bucket, 9 empty
Invoke-Chat '/give @s levitate:wrench 1'
Invoke-Chat '/give @s levitate:andesite_casing 4'
Invoke-Chat '/give @s cobblestone 16'
Invoke-Chat '/give @s iron_chain 16'
Invoke-Chat '/give @s water_bucket 1'
Start-Sleep -Milliseconds 500

# --- belt: casing, then the wrench takes it off again ----------------------------------------------
Invoke-Chat '/setblock 3 -60 4 levitate:kinetics_test_source ["levitate:axis"=2]'
Invoke-Chat '/setblock 3 -60 5 levitate:belt ["levitate:facing"=3,"levitate:part"=0]'
Invoke-Chat '/fill 4 -60 5 6 -60 5 levitate:belt ["levitate:facing"=3,"levitate:part"=1]'
Invoke-Chat '/setblock 7 -60 5 levitate:belt ["levitate:facing"=3,"levitate:part"=2]'
Start-Sleep -Seconds 1
Invoke-Chat '/tp @s 5.5 -60 4.5 0 60'   # south, steeply down: the crosshair is on the belt at 5 -60 5
Start-Sleep -Milliseconds 800
Press-Key '2'
Start-Sleep -Milliseconds 500
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'belts: .* encased the belt at' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 800
Save-Screenshot 'belt-encased' | Out-Null
Press-Key '1'
Start-Sleep -Milliseconds 600
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'belts: .* removed the casing at' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 800
Save-Screenshot 'belt-wrenched' | Out-Null
if (Get-LeviLog -Since $mark | Select-String 'wrench: .* (rotated|dismantled) levitate:belt') { throw 'the generic wrench also handled the belt click' }
Invoke-Chat '/fill 3 -60 4 7 -60 5 air'

# --- deployer: the front face takes the held stack -------------------------------------------------
Invoke-Chat '/tp @s 0.5 -60 0.5 facing 10.5 -59.6 0.5'
Invoke-Chat '/setblock 5 -59 0 levitate:deployer ["levitate:facing"=4,"levitate:axis_along_first"=0]'
Invoke-Chat '/setblock 5 -59 -1 levitate:kinetics_test_source ["levitate:axis"=2]'
Start-Sleep -Seconds 1
Press-Key '3'
Invoke-Chat '/tp @s 0.5 -60 0.5 facing 5.0 -60.0 0.5'
Start-Sleep -Milliseconds 800
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'deployer at .* now holds minecraft:cobblestone' 20 -Since $mark | Out-Null
Save-Screenshot 'deployer' | Out-Null
Invoke-Chat '/fill 5 -59 -1 5 -59 0 air'

# --- chain conveyor: chains select it ---------------------------------------------------------------
Invoke-Chat '/setblock 3 -60 6 levitate:chain_conveyor'
Invoke-Chat '/setblock 3 -61 6 levitate:kinetics_test_source ["levitate:axis"=1]'
Press-Key '4'
Start-Sleep -Milliseconds 600
Invoke-Chat '/tp @s 3.5 -60 2.5 0 17'
Start-Sleep -Milliseconds 1500
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'packages: .* selected the chain conveyor at 0:3:-60:6' 20 -Since $mark | Out-Null
Save-Screenshot 'chain' | Out-Null
Invoke-Chat '/setblock 3 -60 6 air'
Invoke-Chat '/setblock 3 -61 6 grass_block'

# --- valve handle: an empty hand turns it ----------------------------------------------------------
Invoke-Chat '/tp @s 0.5 -60 0.5'
Start-Sleep -Milliseconds 500
Invoke-Chat '/setblock ~3 ~ ~-2 levitate:red_valve_handle ["levitate:facing"=1]'
Press-Key '9'
Start-Sleep -Milliseconds 500
Invoke-Chat '/tp @s ~ ~ ~ -124 21'
Start-Sleep -Milliseconds 800
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'valve handle at .* turned 45 degrees by' 20 -Since $mark | Out-Null
Save-Screenshot 'valve' | Out-Null
Invoke-Chat '/setblock ~3 ~ ~-2 air'

# --- seat: sit down, stand up again ----------------------------------------------------------------
Invoke-Chat '/setblock 2 -60 0 levitate:yellow_seat'
Invoke-Chat '/tp @s 0.5 -60 0.5 -90 32'
Start-Sleep -Milliseconds 800
$mark = Get-LogMark
Click 0.5 0.5 -Relative -Right
Wait-LogLine 'actors: minecraft:player sat down on the seat at' 20 -Since $mark | Out-Null
Start-Sleep -Milliseconds 800
Save-Screenshot 'seat' | Out-Null
Press-Key 'SHIFT' 400
Start-Sleep -Milliseconds 800
Invoke-Chat '/setblock 2 -60 0 air'

# --- a comparator reading a fluid tank lights a lamp ------------------------------------------------
Invoke-Chat '/setblock 3 -60 0 levitate:fluid_tank'
Invoke-Chat '/setblock 4 -60 0 unpowered_comparator ["minecraft:cardinal_direction"="west"]'
Invoke-Chat '/setblock 5 -60 0 redstone_lamp'
Start-Sleep -Seconds 1
Press-Key '5'
Invoke-Chat '/tp @s 0.5 -60 0.5 -90 24'
Start-Sleep -Milliseconds 800
Click 0.5 0.5 -Relative -Right
Start-Sleep -Seconds 2
Invoke-Chat '/tp @s 4.5 -60 -2.5 0 30'
Start-Sleep -Seconds 1
Save-Screenshot 'tank-lamp' | Out-Null
Invoke-Chat '/fill 3 -60 0 5 -60 0 air'

Assert-NoErrors
