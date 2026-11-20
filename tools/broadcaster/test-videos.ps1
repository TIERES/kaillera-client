param([string]$Session = '232-1794875067', [string]$Game = 'WE PW26 INTERNATIONAL - WE2002.chd',
      [int]$StartFrame = 1800, [int]$StopFrame = 4500, [string]$Ending = 'fim',
      [string]$Agent = 'C:\broadcaster\agent\agent.ps1', [int]$MaxMinutes = 10)
# Videos around the game ("aguarde" -> game -> "fim"/"interrompida") without YouTube: an
# RTMP receiver on this machine stands in for it (one "ffmpeg -listen" per sender, each
# saved to out\teste-videos\NN.flv), and the agent's own job code runs one job with the
# YouTube calls replaced. The end shows when each sender came and went (the gaps).
# -Ending auto = no word from the site (RetroArch's reason decides).
$ErrorActionPreference = 'Stop'
Set-Location C:\broadcaster
$OutDir = 'C:\broadcaster\out\teste-videos'
Remove-Item -Recurse -Force $OutDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $OutDir | Out-Null
$Rtmp = 'rtmp://127.0.0.1:19350/live/teste'
$TestLog = Join-Path $OutDir 'agente.log'

# The receiver: a new "ffmpeg -listen" as soon as the previous sender left (its PID in
# receptor.pid, to stop it at the end without taskkill /T - WMI, "Acesso negado" over SSH).
$recv = @"
`$n = 0
while (-not (Test-Path '$OutDir\parar')) {
    `$n++
    `$p = Start-Process ffmpeg -PassThru -NoNewWindow -RedirectStandardError ('$OutDir\receptor-{0:D2}.log' -f `$n) ``
        -ArgumentList ('-hide_banner -loglevel error -listen 1 -i $Rtmp -c copy -f flv "$OutDir\{0:D2}.flv"' -f `$n)
    Set-Content '$OutDir\receptor.pid' `$p.Id
    `$p.WaitForExit()
}
"@
$recvProc = Start-Process powershell -ArgumentList '-NoProfile', '-Command', $recv -PassThru -WindowStyle Hidden
Start-Sleep 2

. $Agent
function Write-Log([string]$msg) {
    $line = '{0} {1}' -f (Get-Date -Format 'HH:mm:ss.fff'), $msg
    Add-Content -Path $TestLog -Value $line -Encoding UTF8
    Write-Host $line
}
function New-Broadcast($job) { return [pscustomobject]@{ id = 'teste'; url = '(local)'; stream_id = 'local'; rtmp = $Rtmp } }
function Set-Thumbnail([string]$id) { Write-Log "(teste) miniatura: $(Test-Path (Join-Path $MediaDir 'miniatura.jpg'))" }
function Complete-Broadcast([string]$id, [bool]$started) { Write-Log "(teste) live encerrada no YouTube (no ar: $started)" }
function Resolve-Game($job) { return (Join-Path $GamesDir $job.game_file) }

foreach ($name in 'aguarde', 'fim', 'interrompida') { $null = Get-Clip $name }   # as the agent does when it starts
$id = 990001
Remove-Item "logs\retroarch-job-$id.log", "logs\ffmpeg-job-$id.log" -ErrorAction SilentlyContinue
$a = [pscustomobject]@{ id = $id; action = 'start'; session_id = $Session; room = ''; game_file = $Game
    start_frame = $StartFrame; stop_frame = $StopFrame; replay = $false; title = 'teste'; description = ''; privacy = 'private'
    iso = $null; ending = $(if ($Ending -and $Ending -ne 'auto') { $Ending } else { $null }) }
Start-Job2 $a
$job = $script:Jobs[$id]
$t0 = Get-Date
while ($job.state -notin 'ended', 'failed' -and ((Get-Date) - $t0).TotalMinutes -lt $MaxMinutes) {
    Step-Job $job
    Start-Sleep -Milliseconds 250
}
if ($job.state -notin 'ended', 'failed') { Write-Log 'TIMEOUT'; Stop-Tree $job.proc; Stop-Intro $job }
Write-Log "estado final: $($job.state) $($job.error)"
Start-Sleep 3
Set-Content "$OutDir\parar" '1'
Stop-Process -Id $recvProc.Id -Force -ErrorAction SilentlyContinue
$last = Get-Content "$OutDir\receptor.pid" -ErrorAction SilentlyContinue
if ($last) { Stop-Process -Id ([int]$last) -Force -ErrorAction SilentlyContinue }   # the one still listening
Start-Sleep 1

"== o que chegou ao receptor (cada arquivo = um envio):"
$prev = $null
foreach ($f in Get-ChildItem $OutDir -Filter '*.flv' | Sort-Object Name) {
    $dur = [double](ffprobe -v error -show_entries format=duration -of csv=p=0 $f.FullName 2>$null)
    $start = $f.CreationTime; $end = $f.LastWriteTime
    $gap = if ($prev) { '{0:N1} s depois do anterior' -f ($start - $prev).TotalSeconds } else { '' }
    "{0}  {1:HH:mm:ss.f} - {2:HH:mm:ss.f}  {3,6:N1} s  {4}" -f $f.Name, $start, $end, $dur, $gap
    $prev = $end
}
"== RetroArch ([WE Camp]):"
Get-Content "logs\retroarch-job-$id.log" -ErrorAction SilentlyContinue | Select-String '\[WE Camp\]' | ForEach-Object { $_.Line }
"== ffmpeg do jogo (envios e erros):"
Get-Content "logs\ffmpeg-job-$id.log" -ErrorAction SilentlyContinue | Where-Object { $_ -match '====|rror|fail' } | Select-Object -Last 12
"== receptor (erros):"
Get-ChildItem $OutDir -Filter 'receptor-*.log' | ForEach-Object { Get-Content $_.FullName } | Select-Object -Last 10
