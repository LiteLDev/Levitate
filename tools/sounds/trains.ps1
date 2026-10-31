# Original sound samples of the trains unit (U27 / U57), synthesised (no recordings, nothing from
# Create). Create's CarriageSounds / train horn have the same roles:
#   pwsh tools/sounds/trains.ps1
#
# train_rolling.wav  one "clack" of wheels over a rail joint (the rolling sound, played faster
#                    and higher as the train speeds up)
# train_steam.wav    a short puff of steam (Create's STEAM: the engine rhythm and the release hiss)
# train_horn.wav     a sustained two-tone train horn (Create's WHISTLE_TRAIN_MANUAL); the low horn
#                    is the same sample an octave down (sounds.d/trains.json)
# train_horn_end.wav the horn's short release (WHISTLE_TRAIN_MANUAL_END)
param([string]$Out = (Join-Path $PSScriptRoot '../../assets/pack/rp/sounds/levitate'))

$rate = 44100
New-Item -ItemType Directory -Force $Out | Out-Null

function Save-Wav([string]$path, [double[]]$samples) {
    $peak = 0.0
    foreach ($s in $samples) { $a = [Math]::Abs($s); if ($a -gt $peak) { $peak = $a } }
    if ($peak -lt 1e-9) { $peak = 1 }
    $gain = 0.85 / $peak
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

$rng = [Random]::new(2757)
function Noise { return $rng.NextDouble() * 2 - 1 }

# rolling: two quick metallic knocks (front and back axle) with a damped ring
$n = [int]($rate * 0.32)
$roll = New-Object double[] $n
foreach ($start in @(0.0, 0.16)) {
    $lp = 0.0
    for ($i = [int]($start * $rate); $i -lt $n; $i++) {
        $t = $i / $rate - $start
        $lp = $lp + 0.35 * ((Noise) - $lp)
        $ring = 0.6 * [Math]::Sin(2 * [Math]::PI * 180 * $t) + 0.3 * [Math]::Sin(2 * [Math]::PI * 410 * $t)
        $env = [Math]::Exp(-$t / 0.035) * [Math]::Min(1, $t / 0.002)
        $roll[$i] += ($lp + $ring) * $env
    }
}
Save-Wav (Join-Path $Out 'train_rolling.wav') $roll

# steam: band-limited hiss with a soft attack and a quick decay
$n = [int]($rate * 0.4)
$steam = New-Object double[] $n
$hp = 0.0; $prev = 0.0; $lp = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $t = $i / $rate
    $x = Noise
    $hp = 0.85 * ($hp + $x - $prev); $prev = $x
    $lp = $lp + 0.5 * ($hp - $lp)
    $env = [Math]::Exp(-$t / 0.12) * [Math]::Min(1, $t / 0.015)
    $steam[$i] = $lp * $env
}
Save-Wav (Join-Path $Out 'train_steam.wav') $steam

# horn: two detuned reed-like tones (a minor third apart), sustained 1 s
function Horn([double]$seconds, [double]$attack, [double]$release) {
    $n = [int]($rate * $seconds)
    $out = New-Object double[] $n
    $p1 = 0.0; $p2 = 0.0
    for ($i = 0; $i -lt $n; $i++) {
        $t = $i / $rate
        $p1 += 2 * [Math]::PI * 311.1 / $rate
        $p2 += 2 * [Math]::PI * 370.0 / $rate
        $s = 0.0
        foreach ($h in 1..6) { $s += ([Math]::Sin($h * $p1) + [Math]::Sin($h * $p2)) / ($h * 1.3) }
        $env = [Math]::Min(1, [Math]::Min($t / $attack, ($seconds - $t) / $release))
        $out[$i] = $s * [Math]::Max(0, $env)
    }
    return $out
}
Save-Wav (Join-Path $Out 'train_horn.wav') (Horn 1.0 0.04 0.05)
Save-Wav (Join-Path $Out 'train_horn_end.wav') (Horn 0.35 0.005 0.3)
Write-Host "train sounds written to $Out"
