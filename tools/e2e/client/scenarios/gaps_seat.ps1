# Train carriage height (U37): the demo train's controls must sit directly on the bogey, for the
# client mod and vanilla clients alike. Seen side-on from 5 blocks south at eye level in front of a
# blue wall, the column through the carriage must not show the wall (or sky) between the bogey and
# the controls. Dot-sourced by gaps.ps1; Test-SeatedColumn is also run offline on screenshots.
# Screenshot: out/gaps/train-side.png

# True when, in the image's centre columns between the rows `top` and `bottom` (fractions of the
# height), the background (blue wall, sky, clouds) never shows again once the carriage started.
function Test-SeatedColumn([string]$png, [double]$top = 0.36, [double]$bottom = 0.56) {
    Add-Type -AssemblyName System.Drawing
    $bmp = [System.Drawing.Bitmap]::FromFile($png)
    try {
        $y0 = [int]($bmp.Height * $top); $y1 = [int]($bmp.Height * $bottom)
        # columns beside the crosshair (it is white, like a cloud), within the bogey's bolster
        foreach ($dx in @(-24, -20, -16, 16, 20, 24)) {
            $x = [int]($bmp.Width / 2) + $dx
            $seenTrain = $false
            for ($y = $y0; $y -le $y1; $y++) {
                $c  = $bmp.GetPixel($x, $y)
                $bg = ($c.B -gt 100 -and $c.R -lt 80 -and $c.G -lt 80) -or ($c.B -gt 150 -and $c.B -gt $c.R + 40) -or ($c.R -gt 215 -and $c.G -gt 215 -and $c.B -gt 215)
                if ($bg -and $seenTrain) { return "background at column $x row $y (between the carriage and the bogey)" }
                if (-not $bg) { $seenTrain = $true }
            }
            if (-not $seenTrain) { return "no carriage in column $x" }
        }
        return $null
    } finally { $bmp.Dispose() }
}

function Assert-TrainSeated {
    $mark = Get-LogMark
    Invoke-Chat '/gamemode creative'
    Invoke-Chat '/clear @s'
    Invoke-Chat '/time set noon'
    Invoke-Chat '/tp @s 0.5 -60 20.5'
    Start-Sleep -Milliseconds 600
    Invoke-Chat '/fill -2 -60 15 64 -54 26 air'
    Invoke-Chat '/fill 2 -60 20 60 -60 20 levitate:track ["levitate:shape"=2]'
    Invoke-Chat '/fill 0 -60 16 16 -52 16 minecraft:blue_concrete'
    Start-Sleep -Milliseconds 800
    Invoke-Chat '/create trains demo_manual'
    Wait-LogLine "trains: assembled train #\d+ '.*' with 1 carriages" 20 -Since $mark | Out-Null
    $at = Wait-LogLine 'trains: demo train #\d+ has its controls at' 20 -Since $mark
    if ($at -notmatch 'controls at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)') { throw "no controls position in '$at'" }
    $inv = [cultureinfo]::InvariantCulture
    $cx = [double]::Parse($Matches[1], $inv); $cz = [double]::Parse($Matches[3], $inv)
    # 5 blocks south of the carriage, eye level, looking north
    $side = '/tp @s {0} -60 {1} 180 0' -f $cx.ToString('0.00', $inv), ($cz + 5).ToString('0.00', $inv)
    Move-Mouse 0.5 0.5 -Relative
    Start-Sleep -Milliseconds 400
    Invoke-Chat $side
    Start-Sleep -Milliseconds 2500
    Save-Screenshot 'train-side' | Out-Null
    $png = Join-Path (Join-Path $script:OutRoot $script:Unit) 'train-side.png'
    $err = Test-SeatedColumn $png
    if ($err) { throw "the carriage floats above its bogey: $err ($png)" }
    Write-Step "the carriage sits on its bogey ($png)"
    Invoke-Chat '/fill 0 -60 16 16 -52 16 air'
    return $mark
}
