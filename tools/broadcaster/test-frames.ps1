param([string]$Session = '36368-1794977438', [string]$Game = 'games\WE PW26 INTERNATIONAL - WE2002.chd',
      [int]$From = 36000, [int]$Stop = 43200, [string]$Out = 'C:\broadcaster\out\teste-frames.flv')
# Tests the catch-up (WECAMP_BROADCAST_FROM_FRAME: fast-forward, not streamed) and the
# control file (stop_frame=N): the output must last about (Stop - From) / 60 seconds.
Set-Location C:\broadcaster
New-Item -ItemType Directory -Force out, run | Out-Null
$ctl = 'C:\broadcaster\run\teste-frames.ctl'
Set-Content -Path $ctl -Value "stop_frame=$Stop" -Encoding ASCII
Remove-Item $Out, logs\retroarch-teste-frames.log, logs\ffmpeg-teste-frames.log -EA SilentlyContinue
$env:WECAMP_BROADCAST_FROM_FRAME = "$From"; $env:WECAMP_CONTROL_FILE = $ctl; $env:WECAMP_LOG_NAME = 'teste-frames'
$t0 = Get-Date
$p = Start-Process cmd -ArgumentList "/c start-broadcast.cmd $Session `"$Game`" $Out" -PassThru -WindowStyle Hidden
$streamAt = $null
while (-not $p.HasExited -and ((Get-Date) - $t0).TotalMinutes -lt 10) {
    Start-Sleep 5
    if (-not $streamAt -and (Select-String -Path logs\retroarch-teste-frames.log -Pattern 'streaming \(ffmpeg' -Quiet -EA SilentlyContinue)) {
        $streamAt = ((Get-Date) - $t0).TotalSeconds; "transmissao comecou aos {0:N0} s (avanco rapido ate o quadro {1})" -f $streamAt, $From
    }
}
if (-not $p.HasExited) { "TIMEOUT"; Get-Process retroarch -EA SilentlyContinue | Stop-Process -Force }
"total: {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds
Get-Content logs\retroarch-teste-frames.log | Select-String 'WE Camp' | % { $_.Line }
if (Test-Path $Out) { ffprobe -v error -show_entries format=duration -of compact $Out }
