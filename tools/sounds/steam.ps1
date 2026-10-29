# Original sound samples of the steam unit (U18), synthesised (no recordings, nothing from Create).
#   pwsh tools/sounds/steam.ps1
#
# steam_whistle.wav  one whistle tone (C5 with a breathy edge), 0.6 s; the whistle sizes and the
#                    extension pitches are this sample played at other pitches (sounds.d/steam.json:
#                    high = pitch 2, medium = 1, low = 0.5, times 2^(-semitones/12) at runtime)
# steam_chiff.wav    the short hiss when a whistle starts
# steam_engine.wav   one puff of the steam engine
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

$rng = [Random]::new(1826)
function Noise { return $rng.NextDouble() * 2 - 1 }

# whistle tone: fundamental + a little octave + filtered breath, gentle vibrato, short fades
$n = [int]($rate * 0.6)
$tone = New-Object double[] $n
$f0 = 523.25
$lp = 0.0
$phase = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $t = $i / $rate
    $vib = 1 + 0.004 * [Math]::Sin(2 * [Math]::PI * 5.5 * $t)
    $phase += 2 * [Math]::PI * $f0 * $vib / $rate
    $lp = $lp + 0.08 * ((Noise) - $lp)
    $s = [Math]::Sin($phase) + 0.25 * [Math]::Sin(2 * $phase) + 0.08 * [Math]::Sin(3 * $phase) + 0.35 * $lp
    $env = [Math]::Min(1, [Math]::Min($t / 0.03, (0.6 - $t) / 0.04))
    $tone[$i] = $s * [Math]::Max(0, $env)
}
Save-Wav (Join-Path $Out 'steam_whistle.wav') $tone

# chiff: a hiss that falls off quickly with a faint tone
$n = [int]($rate * 0.18)
$chiff = New-Object double[] $n
$hp = 0.0; $prev = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $t = $i / $rate
    $x = Noise
    $hp = 0.7 * ($hp + $x - $prev); $prev = $x
    $env = [Math]::Exp(-$t / 0.05) * [Math]::Min(1, $t / 0.004)
    $chiff[$i] = ($hp + 0.3 * [Math]::Sin(2 * [Math]::PI * $f0 * $t)) * $env
}
Save-Wav (Join-Path $Out 'steam_chiff.wav') $chiff

# engine puff: low, soft burst of steam
$n = [int]($rate * 0.3)
$puff = New-Object double[] $n
$lp = 0.0; $lp2 = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $t = $i / $rate
    $lp = $lp + 0.15 * ((Noise) - $lp)
    $lp2 = $lp2 + 0.3 * ($lp - $lp2)
    $env = [Math]::Exp(-$t / 0.08) * [Math]::Min(1, $t / 0.01)
    $puff[$i] = $lp2 * $env
}
Save-Wav (Join-Path $Out 'steam_engine.wav') $puff
Write-Host "steam sounds written to $Out"
