# A 3 minute session with your own microphone for the true end's word (docs/formats/voice.md). It runs
# pt.exe --voice-listen, which opens the microphone chosen in pt.ini (or the system default), tells you in this window
# what to say and when, prints what the recognizer heard, and stops by itself. The log and every segment the
# recognizer heard (wav) are kept in a new folder under %TEMP%, whose path is printed at the end. Nothing is uploaded.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\voice_mic_session.ps1 [-Exe <pt.exe>] [-Seconds 175] [-Dry]
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\build\release\pt.exe'),
    [int]$Seconds = 175,
    [switch]$Dry  # no microphone: SDL's dummy recording device, to test this script
)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
$out = Join-Path $env:TEMP ('pt-voice-session-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $out | Out-Null

$device = ''
$ini = Join-Path $env:APPDATA 'pt-port\pt\pt.ini'
if (Test-Path $ini) {
    $section = ''
    foreach ($line in Get-Content $ini) {
        if ($line -match '^\s*\[(.+)\]') { $section = $Matches[1] }
        elseif ($section -eq 'voice' -and $line -match '^\s*device\s*=\s*"?([^"]*)"?') { $device = $Matches[1] }
    }
}
Write-Host ('Microphone: ' + $(if ($device) { $device } else { 'system default' }))
Write-Host 'Keep this window visible. Each line starting with >>> tells you what to say.'
Write-Host ''

$env:PT_VOICE_DUMP = $out
if ($Dry) { $env:SDL_AUDIO_DRIVER = 'dummy' } else { Remove-Item Env:SDL_AUDIO_DRIVER -ErrorAction SilentlyContinue }
$arguments = @('--voice-listen', $Seconds, '--log', ('"' + (Join-Path $out 'pt.log') + '"'))
if ($device) { $arguments += @('--voice-device', ('"' + $device + '"')) }
# pt.exe is a windowed program, so its console lines go to a file that this window prints as they come
$console = Join-Path $out 'console.txt'
$process = Start-Process -FilePath $Exe -ArgumentList $arguments -WorkingDirectory $out -NoNewWindow -PassThru `
    -RedirectStandardOutput $console -RedirectStandardError (Join-Path $out 'stderr.txt')
$shown = 0
while ($true) {
    $done = $process.HasExited
    if (Test-Path $console) {
        $lines = @(Get-Content $console)
        for ($i = $shown; $i -lt $lines.Count; $i++) { Write-Host $lines[$i] }
        $shown = $lines.Count
    }
    if ($done) { break }
    Start-Sleep -Milliseconds 200
}
Remove-Item Env:PT_VOICE_DUMP

Write-Host ''
Write-Host "Results: $out"
