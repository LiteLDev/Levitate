# Display unit (U24): every display selftest (registries, board structure and lines, nixie rows
# and redstone, link transfer / redstone control / screen, sign target, clipboard screen and
# settings copy, the link item through a simulated player), then a link built from the console:
# a nixie tube read by a display link that writes onto a display board.
$r = Invoke-SelfTest 'display.*'
if ($r.Pass -lt 11) { throw "expected at least 11 display selftests to pass, got $($r.Pass)" }

Send-Console 'tickingarea add 0 -64 0 15 -40 15 lv_display_e2e true'
Wait-Seconds 3
Send-Console 'setblock 3 -58 6 levitate:nixie_tube'
Send-Console 'setblock 3 -57 6 levitate:display_link ["minecraft:block_face"="up"]'
Send-Console 'setblock 6 -58 6 levitate:display_board'
Wait-Seconds 1
$mark = Get-LogMark
Send-Console 'create display text 3 -58 6 0 42'
Wait-LogLine "display text '42' written at 0:3:-58:6" 20 -Since $mark | Out-Null
Send-Console 'create display bind 3 -57 6 6 -58 6 nixie_tube'
Wait-LogLine 'display link at 0:3:-57:6 bound to 6 -58 6 \(nixie_tube\), update sent' 20 -Since $mark | Out-Null
Send-Console 'setblock 3 -57 6 air'
Send-Console 'setblock 3 -58 6 air'
Send-Console 'setblock 6 -58 6 air'
Send-Console 'tickingarea remove lv_display_e2e'
Wait-Seconds 1
