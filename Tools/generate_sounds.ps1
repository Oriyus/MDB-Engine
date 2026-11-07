# Synthesizes the engine's example sound effects into Assets/Sounds (16-bit mono WAV, 44.1 kHz).
# Deterministic (fixed random seed), so re-running gives the same files.
#   powershell -ExecutionPolicy Bypass -File Tools/generate_sounds.ps1
$ErrorActionPreference = 'Stop'
$rate = 44100
$out = Join-Path $PSScriptRoot '..\Assets\Sounds'
New-Item -ItemType Directory -Force $out | Out-Null
$rng = New-Object System.Random 1234
$TwoPi = 2.0 * [Math]::PI

function New-Buffer([double]$seconds) { New-Object double[] ([int]($seconds * $rate)) }

# Peak-normalise to `peak` and write a PCM WAV
function Save-Wav([double[]]$s, [string]$name, [double]$peak = 0.8) {
    $max = 1e-9
    foreach ($v in $s) { $a = [Math]::Abs($v); if ($a -gt $max) { $max = $a } }
    $k = $peak / $max
    $path = Join-Path $out $name
    $fs = [System.IO.File]::Create($path)
    $w = New-Object System.IO.BinaryWriter $fs
    $bytes = $s.Length * 2
    $w.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $w.Write([int](36 + $bytes))
    $w.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt ')); $w.Write([int]16)
    $w.Write([int16]1); $w.Write([int16]1); $w.Write([int]$rate); $w.Write([int]($rate * 2))
    $w.Write([int16]2); $w.Write([int16]16)
    $w.Write([Text.Encoding]::ASCII.GetBytes('data')); $w.Write([int]$bytes)
    foreach ($v in $s) {
        $x = [Math]::Max(-1.0, [Math]::Min(1.0, $v * $k))
        $w.Write([int16]([Math]::Round($x * 32767)))
    }
    $w.Close()
    Write-Host ("{0,-16} {1,6:N2} s" -f $name, ($s.Length / $rate))
}

# Add a tone: frequency can glide from f0 to f1, decays exponentially, with a short attack.
# `bright` mixes in odd harmonics (0 = pure sine).
function Add-Tone([double[]]$s, [double]$start, [double]$len, [double]$f0, [double]$f1,
                  [double]$decay, [double]$gain = 1.0, [double]$bright = 0.0) {
    $i0 = [int]($start * $rate); $n = [int]($len * $rate); $phase = 0.0
    for ($i = 0; $i -lt $n -and ($i0 + $i) -lt $s.Length; $i++) {
        $t = $i / $rate
        $f = $f0 + ($f1 - $f0) * ($t / $len)
        $phase += $TwoPi * $f / $rate
        $env = [Math]::Min(1.0, $t / 0.004) * [Math]::Exp(-$t * $decay)
        $v = [Math]::Sin($phase) + $bright * ([Math]::Sin(3 * $phase) / 3 + [Math]::Sin(5 * $phase) / 5)
        $s[$i0 + $i] += $gain * $env * $v
    }
}

# ---- coin: two bright notes, B5 then E6
$s = New-Buffer 0.4
Add-Tone $s 0.00 0.09 988 988 18 0.7 0.3
Add-Tone $s 0.07 0.33 1319 1319 9 1.0 0.3
Save-Wav $s 'coin.wav'

# ---- jump: quick upward glide
$s = New-Buffer 0.2
Add-Tone $s 0 0.2 260 620 14 1.0 0.5
Save-Wav $s 'jump.wav' 0.6

# ---- bounce: a "boing" that rises fast and wobbles
$s = New-Buffer 0.5; $phase = 0.0
for ($i = 0; $i -lt $s.Length; $i++) {
    $t = $i / $rate
    $f = (180 + 520 * (1 - [Math]::Exp(-$t * 14))) * (1 + 0.07 * [Math]::Sin($TwoPi * 28 * $t))
    $phase += $TwoPi * $f / $rate
    $s[$i] = [Math]::Min(1.0, $t / 0.003) * [Math]::Exp(-$t * 6) * ([Math]::Sin($phase) + 0.25 * [Math]::Sin(2 * $phase))
}
Save-Wav $s 'bounce.wav'

# ---- thud: a low knock with a short click on top
$s = New-Buffer 0.25
Add-Tone $s 0 0.25 120 50 22 1.0
for ($i = 0; $i -lt [int](0.012 * $rate); $i++) { $s[$i] += 0.5 * ($rng.NextDouble() * 2 - 1) * (1 - $i / (0.012 * $rate)) }
Save-Wav $s 'thud.wav'

# ---- lava hit: a hiss (high-passed noise) over a low rumble
$s = New-Buffer 0.7; $prev = 0.0
for ($i = 0; $i -lt $s.Length; $i++) {
    $t = $i / $rate
    $n = $rng.NextDouble() * 2 - 1
    $hiss = $n - $prev; $prev = $n                     # first difference: strips the lows
    $s[$i] = $hiss * [Math]::Min(1.0, $t / 0.01) * [Math]::Exp(-$t * 4.5) * 0.6
}
Add-Tone $s 0 0.5 90 45 7 0.8
Save-Wav $s 'lava_hit.wav'

# ---- lava loop: brown-noise rumble with random bubble pops; the end crossfades into the start
$len = 3.0; $fade = 0.4
$s = New-Buffer ($len + $fade); $b = 0.0
for ($i = 0; $i -lt $s.Length; $i++) {
    $b = ($b + 0.02 * ($rng.NextDouble() * 2 - 1)) * 0.995   # leaky integrator: brown noise
    $s[$i] = $b * 4
}
for ($k = 0; $k -lt 14; $k++) {
    $at = $rng.NextDouble() * ($len + $fade - 0.15)
    $f = 70 + $rng.NextDouble() * 90
    Add-Tone $s $at 0.15 ($f * 1.6) $f 25 (0.25 + 0.25 * $rng.NextDouble())
}
$n = [int]($len * $rate); $nf = [int]($fade * $rate)
$loop = New-Object double[] $n
for ($i = 0; $i -lt $n; $i++) { $loop[$i] = $s[$i] }
for ($i = 0; $i -lt $nf; $i++) {                           # blend the tail over the head
    $w = $i / $nf
    $loop[$i] = $s[$i] * $w + $s[$n + $i] * (1 - $w)
}
Save-Wav $loop 'lava_loop.wav' 0.7

# ---- unlock: a quick rising G-major arpeggio
$s = New-Buffer 0.7
Add-Tone $s 0.00 0.30 784 784 8 0.8 0.2
Add-Tone $s 0.08 0.30 988 988 8 0.8 0.2
Add-Tone $s 0.16 0.54 1175 1175 5 1.0 0.2
Save-Wav $s 'unlock.wav'

# ---- win: C E G C up, then the top note rings
$s = New-Buffer 1.4
$notes = 523.25, 659.25, 783.99
for ($k = 0; $k -lt 3; $k++) { Add-Tone $s (0.13 * $k) 0.25 $notes[$k] $notes[$k] 9 0.8 0.4 }
Add-Tone $s 0.39 1.0 1046.5 1046.5 3 1.0 0.4
Add-Tone $s 0.39 1.0 523.25 523.25 3 0.5 0.4
Save-Wav $s 'win.wav'

# ---- lose: three notes sagging downwards
$s = New-Buffer 1.2
Add-Tone $s 0.00 0.35 392 380 6 1.0 0.5
Add-Tone $s 0.30 0.35 330 318 6 1.0 0.5
Add-Tone $s 0.60 0.60 262 240 4 1.0 0.5
Save-Wav $s 'lose.wav'
