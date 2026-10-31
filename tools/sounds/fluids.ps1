# Original sound samples of the fluids unit (U19), synthesised (no recordings, nothing from Create).
#   pwsh tools/sounds/fluids.ps1
#
# spout_1..3.wav  the spurt when a spout starts filling (Create's SPOUTING has three variants):
#                 a short burst of wet, low-passed noise with a few falling bubble tones
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

# variant: seed, length (s), noise cutoff (0..1), bubble start frequencies (Hz)
$variants = @(
    @{ Seed = 611; Length = 0.42; Cut = 0.20; Bubbles = @(880, 620, 1150) },
    @{ Seed = 612; Length = 0.38; Cut = 0.26; Bubbles = @(760, 990) },
    @{ Seed = 613; Length = 0.46; Cut = 0.17; Bubbles = @(940, 700, 520, 1240) }
)

for ($v = 0; $v -lt $variants.Count; $v++) {
    $spec = $variants[$v]
    $rng = [Random]::new($spec.Seed)
    $n = [int]($rate * $spec.Length)
    $wave = New-Object double[] $n
    $lp = 0.0; $lp2 = 0.0
    # bubbles: start time, frequency, phase
    $bubbles = @()
    for ($b = 0; $b -lt $spec.Bubbles.Count; $b++) {
        $bubbles += @{ Start = 0.02 + $rng.NextDouble() * ($spec.Length * 0.6); F = $spec.Bubbles[$b]; Phase = 0.0 }
    }
    for ($i = 0; $i -lt $n; $i++) {
        $t = $i / $rate
        $x = $rng.NextDouble() * 2 - 1
        $lp = $lp + $spec.Cut * ($x - $lp)
        $lp2 = $lp2 + 0.5 * ($lp - $lp2)
        # gushing noise: quick attack, a wobble, then a decay
        $env = [Math]::Min(1, $t / 0.008) * [Math]::Exp(-$t / ($spec.Length * 0.35)) * (0.8 + 0.2 * [Math]::Sin(2 * [Math]::PI * 23 * $t))
        $s = $lp2 * $env
        foreach ($bb in $bubbles) {
            $dt = $t - $bb.Start
            if ($dt -lt 0 -or $dt -gt 0.07) { continue }
            # a bubble: a sine whose pitch rises as it closes, fading fast
            $f = $bb.F * (1 + 2.5 * $dt)
            $bb.Phase += 2 * [Math]::PI * $f / $rate
            $s += 0.35 * [Math]::Sin($bb.Phase) * [Math]::Exp(-$dt / 0.018) * [Math]::Min(1, $dt / 0.002)
        }
        $wave[$i] = $s
    }
    Save-Wav (Join-Path $Out ("fluids_spout_{0}.wav" -f ($v + 1))) $wave
}
Write-Host "fluids sounds written to $Out"
