# Original sound samples of the U37 `gaps` unit (kinetic ambience), synthesised (no recordings,
# nothing from Create or Minecraft).
#   pwsh tools/sounds/gaps.ps1
#
# kinetics_cogs.wav    0.5 s of wooden gear teeth meshing (10 evenly spaced clicks, so the sample
#                      repeats seamlessly): Create's COG soundscape (cogwheels, large cogwheels,
#                      encased cogs, gearboxes). Played again every 10 ticks at the soundscape's pitch.
# kinetics_rumble.wav  0.5 s of a soft low mechanical rumble that loops seamlessly (the period
#                      fits the sample): Create's KINETIC soundscape (every rotating block, very quiet).
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

$rng = [Random]::new(3737)
function Noise { return $rng.NextDouble() * 2 - 1 }

# cogs: each tooth is a short noise tick through two wooden resonances (a struck block of wood),
# slightly varied in strength and tone; the last click has died away before the sample ends
$len = [int]($rate * 0.5)
$cogs = New-Object double[] $len
$clicks = 10
for ($c = 0; $c -lt $clicks; $c++) {
    $start = [int]($c * $len / $clicks + $rng.Next(0, 120))
    $amp = 0.7 + 0.3 * $rng.NextDouble()
    $f1 = 820 + 90 * $rng.NextDouble()
    $f2 = 1960 + 240 * $rng.NextDouble()
    $n = [int]($rate * 0.035)
    $hp = 0.0; $prev = 0.0
    for ($i = 0; $i -lt $n -and $start + $i -lt $len; $i++) {
        $t = $i / $rate
        $x = Noise
        $hp = 0.6 * ($hp + $x - $prev); $prev = $x
        $body = 0.8 * [Math]::Sin(2 * [Math]::PI * $f1 * $t) * [Math]::Exp(-$t / 0.009) + 0.45 * [Math]::Sin(2 * [Math]::PI * $f2 * $t) * [Math]::Exp(-$t / 0.004)
        $tick = $hp * [Math]::Exp(-$t / 0.0015)
        $cogs[$start + $i] += $amp * ($body + 0.6 * $tick) * [Math]::Min(1, $t / 0.0004)
    }
}
Save-Wav (Join-Path $Out 'kinetics_cogs.wav') $cogs

# rumble: low-passed noise with a 60 Hz / 120 Hz hum (30 and 60 periods in 0.5 s: seamless loop);
# the noise is cross-faded into its own start so the seam doesn't click
$len = [int]($rate * 0.5)
$xf = [int]($rate * 0.05)
$noise = New-Object double[] ($len + $xf)
$lp = 0.0; $lp2 = 0.0
for ($i = 0; $i -lt $noise.Length; $i++) {
    $lp = $lp + 0.02 * ((Noise) - $lp)
    $lp2 = $lp2 + 0.05 * ($lp - $lp2)
    $noise[$i] = $lp2
}
$rumble = New-Object double[] $len
for ($i = 0; $i -lt $len; $i++) {
    $t = $i / $rate
    $n = $noise[$i]
    if ($i -lt $xf) { $k = $i / $xf; $n = $k * $noise[$i] + (1 - $k) * $noise[$len + $i] }
    $rumble[$i] = 4 * $n + 0.35 * [Math]::Sin(2 * [Math]::PI * 60 * $t) + 0.15 * [Math]::Sin(2 * [Math]::PI * 120 * $t)
}
Save-Wav (Join-Path $Out 'kinetics_rumble.wav') $rumble 0.7
Write-Host "gaps sounds written to $Out"
