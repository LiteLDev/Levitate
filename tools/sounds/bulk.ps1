# Original sound sample of the U13 `bulk` unit, synthesised (no recordings, nothing from Create or
# Minecraft).
#   pwsh tools/sounds/bulk.ps1
#
# fan_wind.wav  0.5 s of airy wind that loops seamlessly (the noise is cross-faded into its own
#               start): Create's air current sound for a player caught in a fan's current
#               (AirCurrent.enableClientPlayerSound). Played again every 10 ticks, louder with the
#               fan's speed, higher with the player's speed.
param([string]$Out = (Join-Path $PSScriptRoot '../../assets/pack/rp/sounds/levitate'))

$rate = 44100
New-Item -ItemType Directory -Force $Out | Out-Null

function Save-Wav([string]$path, [double[]]$samples, [double]$level = 0.85) {
    $peak = 0.0
    foreach ($s in $samples) { $a = [Math]::Abs($s); if ($a -gt $peak) { $peak = $a } }
    if ($peak -lt 1e-9) { $peak = 1 }
    $gain = $level / $peak
    $data = New-Object byte[] ($samples.Length * 2)
    for ($i = 0; $i -lt $samples.Length; $i++) {
        $v = [int][Math]::Round([Math]::Max(-1, [Math]::Min(1, $samples[$i] * $gain)) * 32767)
        $data[2 * $i] = [byte]($v -band 0xff)
        $data[2 * $i + 1] = [byte](($v -shr 8) -band 0xff)
    }
    $ms = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter $ms
    $w.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $w.Write([int](36 + $data.Length)); $w.Write([Text.Encoding]::ASCII.GetBytes('WAVE'))
    $w.Write([Text.Encoding]::ASCII.GetBytes('fmt ')); $w.Write([int]16); $w.Write([int16]1); $w.Write([int16]1)
    $w.Write([int]$rate); $w.Write([int]($rate * 2)); $w.Write([int16]2); $w.Write([int16]16)
    $w.Write([Text.Encoding]::ASCII.GetBytes('data')); $w.Write([int]$data.Length); $w.Write($data)
    $w.Flush()
    [IO.File]::WriteAllBytes($path, $ms.ToArray())
    $w.Dispose()
}

$rng = [Random]::new(1313)
function Noise { return $rng.NextDouble() * 2 - 1 }

# wind: white noise through a slowly sweeping two-pole band-pass (a gust that rises and falls once
# per sample, so the loop breathes), plus a little low rumble; the end is cross-faded into the start
$len = [int]($rate * 0.5)
$xf = [int]($rate * 0.06)
$total = $len + $xf
$raw = New-Object double[] $total
$y1 = 0.0; $y2 = 0.0; $lp = 0.0
for ($i = 0; $i -lt $total; $i++) {
    $t = ($i % $len) / $len
    $f = 520 + 380 * (0.5 - 0.5 * [Math]::Cos(2 * [Math]::PI * $t))
    $r = 0.985
    $c = 2 * $r * [Math]::Cos(2 * [Math]::PI * $f / $rate)
    $x = Noise
    $y = (1 - $r) * $x + $c * $y1 - $r * $r * $y2
    $y2 = $y1; $y1 = $y
    $lp = $lp + 0.01 * ((Noise) - $lp)
    $gust = 0.65 + 0.35 * (0.5 - 0.5 * [Math]::Cos(2 * [Math]::PI * $t))
    $raw[$i] = $gust * $y + 0.6 * $lp
}
$wind = New-Object double[] $len
for ($i = 0; $i -lt $len; $i++) {
    $v = $raw[$i]
    if ($i -lt $xf) { $k = $i / $xf; $v = $k * $raw[$i] + (1 - $k) * $raw[$len + $i] }
    $wind[$i] = $v
}
Save-Wav (Join-Path $Out 'fan_wind.wav') $wind 0.6
Write-Host "bulk sounds written to $Out"
