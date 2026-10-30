# Original sound samples of the equipment unit (U31), synthesised (no recordings, nothing from Create).
#   pwsh tools/sounds/equipment.ps1
#
# potato_cannon_fwoomp.wav  the potato cannon's launch (Create's "Resonant fwoomp" event): a puff of
#                           air through a tube, a falling resonant body and a soft low thump, 0.45 s.
#                           Played at the ammo type's pitch (sounds.d/equipment.json; pitch at runtime).
# The hit sound (Create's potato_hit) layers two vanilla sounds, as Create does (sounds.d/equipment.json).
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

$rng = [Random]::new(3131)
function Noise { return $rng.NextDouble() * 2 - 1 }

# fwoomp: three layers
#  - air: noise through a resonant band-pass (a two-pole filter) whose centre falls 900 -> 250 Hz
#  - body: a sine falling 180 -> 70 Hz with a little second harmonic (the tube's resonance)
#  - thump: a short 55 Hz pulse at the start
$n = [int]($rate * 0.45)
$fw = New-Object double[] $n
$y1 = 0.0; $y2 = 0.0
$phase = 0.0
for ($i = 0; $i -lt $n; $i++) {
    $t = $i / $rate
    $u = $t / 0.45
    # resonant band-pass on noise
    $fc = 250 + 650 * [Math]::Exp(-$t / 0.06)
    $r = 0.985
    $theta = 2 * [Math]::PI * $fc / $rate
    $x = Noise
    $y = (1 - $r) * $x + 2 * $r * [Math]::Cos($theta) * $y1 - $r * $r * $y2
    $y2 = $y1; $y1 = $y
    $airEnv = [Math]::Min(1, $t / 0.008) * [Math]::Exp(-$t / 0.09)
    # falling body
    $f = 70 + 110 * [Math]::Exp(-$t / 0.07)
    $phase += 2 * [Math]::PI * $f / $rate
    $bodyEnv = [Math]::Min(1, $t / 0.012) * [Math]::Exp(-$t / 0.12)
    $body = [Math]::Sin($phase) + 0.3 * [Math]::Sin(2 * $phase)
    # thump
    $thump = [Math]::Sin(2 * [Math]::PI * 55 * $t) * [Math]::Exp(-$t / 0.025)
    $fade = [Math]::Min(1, (1 - $u) / 0.1)
    $fw[$i] = (6.0 * $y * $airEnv + 0.8 * $body * $bodyEnv + 0.6 * $thump) * [Math]::Max(0, $fade)
}
Save-Wav (Join-Path $Out 'potato_cannon_fwoomp.wav') $fw
Write-Host "equipment sounds written to $Out"
