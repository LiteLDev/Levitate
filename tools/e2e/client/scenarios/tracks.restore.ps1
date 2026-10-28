# Undoes tracks.prelaunch.ps1: the content log file goes back off.
foreach ($o in Get-ChildItem -Recurse -Path (Get-GameDataDir) -Filter 'options.txt' -ErrorAction SilentlyContinue) {
    $lines = Get-Content $o.FullName
    if ($lines -match '^content_log_file:1') {
        $lines -replace '^content_log_file:1', 'content_log_file:0' | Set-Content $o.FullName
        Write-Step "content log file disabled in $($o.FullName)"
    }
}
