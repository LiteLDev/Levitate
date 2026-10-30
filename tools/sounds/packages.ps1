# Original sound samples of the packages unit (U25), synthesised (no recordings, nothing from Create).
#   pwsh tools/sounds/packages.ps1
#
# packages_packager.wav         the packager boxing / unboxing: cardboard rustle over a soft clunk
# packages_package_pop.wav      a package opened or broken: a dull cardboard pop with a flap
# packages_depot_plop.wav       a package set down: a soft, short plop
# packages_frogport_open.wav    the frogport opening its mouth: a wet, rising blop
# packages_frogport_catch.wav   the tongue catching a package: a quick slurp ending in a smack
# packages_frogport_deposit.wav a package dropped into the frogport: a low gulp
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

$rng = [Random]::new(2525)
function Noise { return $rng.NextDouble() * 2 - 1 }

# A pitch-swept sine "blop" (start/end frequency, length, decay), used by the plops and gulps.
function Blop([double]$f0, [double]$f1, [double]$len, [double]$decay, [double]$attack = 0.003) {
    $n = [int]($rate * $len)
    $out = New-Object double[] $n
    $phase = 0.0
    for ($i = 0; $i -lt $n; $i++) {
        $t = $i / $rate
        $k = $t / $len
        $f = $f0 * [Math]::Pow($f1 / $f0, $k)
        $phase += 2 * [Math]::PI * $f / $rate
        $env = [Math]::Exp(-$t / $decay) * [Math]::Min(1, $t / $attack)
        $out[$i] = ([Math]::Sin($phase) + 0.2 * [Math]::Sin(2 * $phase)) * $env
    }
    return , $out
}

function Mix([double[]]$a, [double[]]$b, [int]$offset, [double]$gain) {
    for ($i = 0; $i -lt $b.Length -and $i + $offset -lt $a.Length; $i++) { $a[$i + $offset] += $gain * $b[$i] }
}

# packager: three short cardboard rustles (band-passed noise bursts) over a soft low clunk
$n = [int]($rate * 0.5)
$pack = New-Object double[] $n
$clunk = Blop 140 70 0.25 0.06
Mix $pack $clunk 0 1.0
foreach ($start in 0.02, 0.13, 0.27) {
    $len = 0.09
    $m = [int]($rate * $len)
    $burst = New-Object double[] $m
    $lp = 0.0; $hp = 0.0; $prev = 0.0
    for ($i = 0; $i -lt $m; $i++) {
        $t = $i / $rate
        $x = Noise
        $hp = 0.85 * ($hp + $x - $prev); $prev = $x
        $lp = $lp + 0.35 * ($hp - $lp)
        $crackle = if ($rng.NextDouble() -lt 0.02) { 2.5 * (Noise) } else { 0 }
        $env = [Math]::Sin([Math]::PI * $t / $len)
        $burst[$i] = ($lp + $crackle) * $env
    }
    Mix $pack $burst ([int]($rate * $start)) 0.6
}
Save-Wav (Join-Path $Out 'packages_packager.wav') $pack

# package pop: a dull thump plus a short papery flap
$n = [int]($rate * 0.2)
$pop = Blop 220 90 0.2 0.035
$flapLen = [int]($rate * 0.06)
$lp = 0.0
for ($i = 0; $i -lt $flapLen; $i++) {
    $t = $i / $rate
    $lp = $lp + 0.5 * ((Noise) - $lp)
    $pop[$i + [int]($rate * 0.01)] += 0.7 * $lp * [Math]::Exp(-$t / 0.015)
}
Save-Wav (Join-Path $Out 'packages_package_pop.wav') $pop

# depot plop: soft and short
Save-Wav (Join-Path $Out 'packages_depot_plop.wav') (Blop 330 160 0.14 0.03)

# frogport open: a wet blop rising in pitch
$open = Blop 260 620 0.16 0.05 0.008
Save-Wav (Join-Path $Out 'packages_frogport_open.wav') $open

# frogport catch: a slurp (noise through a rising resonance) ending in a smack
$n = [int]($rate * 0.3)
$catch = New-Object double[] $n
$y1 = 0.0; $y2 = 0.0
for ($i = 0; $i -lt [int]($rate * 0.22); $i++) {
    $t = $i / $rate
    $f = 400 + 1800 * ($t / 0.22)
    $r = 0.97
    $c = 2 * $r * [Math]::Cos(2 * [Math]::PI * $f / $rate)
    $y = (Noise) * 0.05 + $c * $y1 - $r * $r * $y2
    $y2 = $y1; $y1 = $y
    $env = [Math]::Min(1, $t / 0.03) * (1 - $t / 0.22)
    $catch[$i] = $y * $env
}
Mix $catch (Blop 700 300 0.08 0.015) ([int]($rate * 0.21)) 2.5
Save-Wav (Join-Path $Out 'packages_frogport_catch.wav') $catch

# frogport deposit: a low gulp falling in pitch
Save-Wav (Join-Path $Out 'packages_frogport_deposit.wav') (Blop 300 110 0.22 0.06 0.006)
