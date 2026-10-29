# Turns the game's content log file on for this run, so the scenario can check that the transmission
# blocks, their geometry and textures load without content errors (transmission.restore.ps1 turns it off).
$script:TransmissionContentLogStart = Get-Date
foreach ($o in Get-ChildItem -Recurse -Path (Get-GameDataDir) -Filter 'options.txt' -ErrorAction SilentlyContinue) {
    $lines = Get-Content $o.FullName
    if ($lines -match '^content_log_file:0') {
        $lines -replace '^content_log_file:0', 'content_log_file:1' | Set-Content $o.FullName
        Write-Step "content log file enabled in $($o.FullName)"
    }
}

# Remote modes: the game caches server resource packs by id + version, and every local build is
# version 0.0.x, so a Levitate RP cached by another branch's run would hide this build's models.
# Drop cached Levitate resource packs so the client downloads the host's pack again.
foreach ($cache in Get-ChildItem -Recurse -Directory -Path (Get-GameDataDir) -Filter 'packcache' -ErrorAction SilentlyContinue) {
    foreach ($pack in Get-ChildItem -Directory -Path (Join-Path $cache.FullName 'resource') -ErrorAction SilentlyContinue) {
        if (Test-Path (Join-Path $pack.FullName 'models/blocks/kinetics_test_cog.geo.json')) {
            Remove-Item -Recurse -Force $pack.FullName
            Write-Step "dropped cached Levitate resource pack $($pack.Name)"
        }
    }
}
