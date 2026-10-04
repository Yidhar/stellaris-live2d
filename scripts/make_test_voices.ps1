# Writes a few short voice lines (WAV, made by Windows' own speech synthesis) for trying the voice playback: a test, not game art.
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\make_test_voices.ps1 [-Out <folder>]
param([string]$Out = (Join-Path (Split-Path -Parent $PSScriptRoot) "scratch\voice"))
Add-Type -AssemblyName System.Speech
New-Item -ItemType Directory -Force $Out | Out-Null
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$zh = $synth.GetInstalledVoices() | Where-Object { $_.VoiceInfo.Culture.Name -eq "zh-CN" } | Select-Object -First 1
if ($zh) { $synth.SelectVoice($zh.VoiceInfo.Name) }
$lines = @(
    @("line1", "哎呀，别戳啦，很痒的！"),
    @("line2", "指挥官，有什么吩咐吗？"),
    @("line3", "嗯？怎么了？")
)
foreach ($l in $lines) {
    $synth.SetOutputToWaveFile((Join-Path $Out ($l[0] + ".wav")))
    $synth.Speak($l[1])
}
$synth.Dispose()
Get-ChildItem $Out | ForEach-Object { "{0}  {1} bytes" -f $_.Name, $_.Length }
