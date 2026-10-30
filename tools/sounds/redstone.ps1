# Original sound sample of the U49 desk bell (redstone unit, U23), synthesised (no recordings,
# nothing from Create or Minecraft).
#   pwsh tools/sounds/redstone.ps1
#
# desk_bell.wav  1.2 s: a reception bell struck once: a short metallic click, then a bright bell
#                tone (inharmonic partials of a small dome bell) ringing out.
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

$rng = [Random]::new(4949)
$len = [int]($rate * 1.2)
$bell = New-Object double[] $len
# partials of a small dome bell: frequency (Hz), level, decay time (s)
$partials = @(
    @(1975, 1.0, 0.55), @(1985, 0.5, 0.60),   # the beating fundamental pair
    @(4380, 0.45, 0.30), @(6930, 0.25, 0.16), @(9810, 0.12, 0.09), @(988, 0.10, 0.40)
)
for ($i = 0; $i -lt $len; $i++) {
    $t = $i / $rate
    $v = 0.0
    foreach ($p in $partials) { $v += $p[1] * [Math]::Sin(2 * [Math]::PI * $p[0] * $t) * [Math]::Exp(-$t / $p[2]) }
    # the plunger's click
    if ($t -lt 0.006) { $v += 0.8 * ($rng.NextDouble() * 2 - 1) * (1 - $t / 0.006) }
    $bell[$i] = $v * [Math]::Min(1, $t / 0.0008)
}
Save-Wav (Join-Path $Out 'desk_bell.wav') $bell 0.8
Write-Host "redstone sounds written to $Out"
