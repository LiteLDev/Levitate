# Turns the game's content log file on for this run, so the scenario can check that the signal
# and observer blocks, their geometry and textures load without content errors (signals.restore.ps1 turns it off).
$script:SignalsContentLogStart = Get-Date
foreach ($o in Get-ChildItem -Recurse -Path (Get-GameDataDir) -Filter 'options.txt' -ErrorAction SilentlyContinue) {
    $lines = Get-Content $o.FullName
    if ($lines -match '^content_log_file:0') {
        $lines -replace '^content_log_file:0', 'content_log_file:1' | Set-Content $o.FullName
        Write-Step "content log file enabled in $($o.FullName)"
    }
}
