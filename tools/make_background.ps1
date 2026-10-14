# Builds models/sound_background.bin: examples of sounds that aren't commands
# (common English words spoken by the Windows text-to-speech voices, plus the
# synthetic noises vht-test adds), used to reject sounds that don't match any
# action well enough.
#
# Usage: tools\make_background.ps1 [-Test build\vht-test.exe] [-Out models\sound_background.bin]
param(
    [string]$Test = "$PSScriptRoot\..\build\vht-test.exe",
    [string]$Out = "$PSScriptRoot\..\models\sound_background.bin"
)

$words = @'
the of and to in is you that it he was for on are as with his they at one have this from or had by word but not
some we can out other were all there when use your how said an each she which do their time if will way about many
then them write would like so these her long make thing see him two has look more day could go come did number sound
most people my over know water than call first who may side been now find any new work part take get place made
live where after back little only round man year came show every good me give our under name very through just form
sentence great think say help low line differ turn cause much mean before move boy old too same tell does set three
want air well also play small end put home read hand port large spell add even land here must big high such follow
act why ask men change went kind off need house picture try us again animal point mother world near build self
earth father head stand own page should country found answer school grow study still learn plant cover food sun four
between state keep eye never last let thought city tree cross farm hard begin might story saw far sea draw late run
dont while press close night real life few north open seem together next white children got example ease paper
group always music those both mark often letter until mile river car feet care second book carry took science eat
room friend began idea fish mountain once base hear horse cut sure watch color face wood main enough plain girl usual
young ready above ever red list though feel talk
'@ -split '\s+' | Where-Object { $_ }

Add-Type -AssemblyName System.Speech
$dir = Join-Path ([System.IO.Path]::GetTempPath()) "vht-background"
New-Item -ItemType Directory -Force $dir | Out-Null
$format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000,
    [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$voices = (New-Object System.Speech.Synthesis.SpeechSynthesizer).GetInstalledVoices() |
    ForEach-Object { $_.VoiceInfo.Name }
$rates = @('-10%', '+0%', '+15%')
$files = @()
for ($i = 0; $i -lt $words.Count; $i++) {
    $synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
    $synth.SelectVoice($voices[$i % $voices.Count])
    $file = Join-Path $dir ("word_{0:D3}.wav" -f $i)
    $synth.SetOutputToWaveFile($file, $format)
    $ssml = "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='en-US'>" +
        "<prosody rate='$($rates[$i % 3])'>$($words[$i])</prosody></speak>"
    $synth.SpeakSsml($ssml)
    $synth.Dispose()
    $files += $file
}
& $Test background $Out @files
Remove-Item -Recurse -Force $dir
