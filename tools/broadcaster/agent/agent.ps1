<#
  WE Camp broadcaster - agent (Windows PowerShell 5.1, nothing to install).

  Loop, every few seconds:
    1. tells the WE Camp site (POST /api/broadcaster/poll) what it is running;
    2. receives what to start / stop;
    3. start: makes sure the match ISO is in games\ (downloads it from the site's
       ISO registry when missing), creates the YouTube live (title/description
       from the site), binds it to this machine's YouTube stream and runs
       start-broadcast.cmd (RetroArch broadcaster -> ffmpeg -> YouTube);
    4. stop: writes "stop_frame=N" to the job's control file - RetroArch ends
       the stream when the game reaches that frame and quits;
    5. reports when RetroArch quits (ended / failed with the log tail).
  Around the game, on the same YouTube stream (one sender at a time):
    - before it, the "Aguarde" video (midia\aguarde.mp4, in a loop) from the moment
      the live is created; RetroArch's ffmpeg is started through ffmpeg-jogo.cmd,
      which stops it right before the game goes out;
    - after it, "fim" (the match ended at the final whistle) or "interrompida"
      (someone left, the host's session ended early, cancelled, failed) - then the
      live is ended on YouTube.
    The live's thumbnail is midia\miniatura.jpg.
  Every hour it also checks the ISO registry (GET /api/broadcaster/isos):
  updated ISOs it holds are downloaded again, and missing ones too
  ("sync_all"), while the disk keeps "min_free_gb" free.

  Usage:
    agent.ps1               run (the scheduled task does this)
    agent.ps1 -Authorize    once, in a desktop session with a browser logged in
                            to the channel's Google account: YouTube consent
    agent.ps1 -Check        config, YouTube token/stream and the site, then exit
#>
param([switch]$Authorize, [switch]$Check, [switch]$Once)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$AgentVersion = 'agent-2026.11.22'
$AgentDir = $PSScriptRoot
$Root = Split-Path $AgentDir -Parent
$StateDir = Join-Path $AgentDir 'state'
$RunDir = Join-Path $Root 'run'
$LogDir = Join-Path $Root 'logs'
foreach ($d in $StateDir, $RunDir, $LogDir) { if (-not (Test-Path $d)) { New-Item -ItemType Directory -Force $d | Out-Null } }
$LogFile = Join-Path $LogDir 'agent.log'

function Write-Log([string]$msg) {
    $line = '{0} {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $msg
    try {
        if ((Test-Path $LogFile) -and (Get-Item $LogFile).Length -gt 5MB) { Move-Item -Force $LogFile ($LogFile + '.1') }
        Add-Content -Path $LogFile -Value $line -Encoding UTF8
    } catch { }
    Write-Host $line
}

# --- config -----------------------------------------------------------------------
$ConfigPath = Join-Path $AgentDir 'config.json'
if (-not (Test-Path $ConfigPath)) { throw "Falta $ConfigPath (copie config.example.json e preencha o token)." }
$Config = Get-Content $ConfigPath -Raw -Encoding UTF8 | ConvertFrom-Json
function Get-Cfg([string]$name, $default) { if ($null -ne $Config.$name -and "$($Config.$name)" -ne '') { return $Config.$name } return $default }
$Server = ([string](Get-Cfg 'server' 'https://we2002.wgs.dev.br')).TrimEnd('/')
$GamesDir = [string](Get-Cfg 'games_dir' (Join-Path $Root 'games'))
$Slots = [int](Get-Cfg 'slots' 1)
$MinFreeGB = [double](Get-Cfg 'min_free_gb' 5)
$SyncAll = [bool](Get-Cfg 'sync_all' $true)
$MaxJobHours = [double](Get-Cfg 'max_job_hours' 3)
$ClientSecretPath = [string](Get-Cfg 'client_secret' '')
$MediaDir = [string](Get-Cfg 'media_dir' (Join-Path $Root 'midia'))
$IntroMaxMinutes = [double](Get-Cfg 'intro_max_minutes' 30)   # an orphan "Aguarde" (agent killed) stops by itself
$EndDelaySeconds = [int](Get-Cfg 'end_delay_seconds' 15)      # after the end video, before ending the live
$FfmpegExe = [string](Get-Cfg 'ffmpeg' '')
if (-not $FfmpegExe) {
    $found = Get-Command ffmpeg.exe -ErrorAction SilentlyContinue
    $FfmpegExe = if ($found) { $found.Source } else {
        'C:\Program Files\WinGet\Packages\Gyan.FFmpeg.Shared_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0.2-full_build-shared\bin\ffmpeg.exe' }
}
$FfprobeExe = Join-Path (Split-Path $FfmpegExe -Parent) 'ffprobe.exe'
$GameFfmpeg = Join-Path $Root 'ffmpeg-jogo.cmd'
if (-not $ClientSecretPath) {
    # The newest one, if a new OAuth client was created later.
    $found = Get-ChildItem (Join-Path $Root 'assets') -Filter 'client_secret_*.json' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($found) { $ClientSecretPath = $found.FullName }
}
if (-not (Test-Path $GamesDir)) { New-Item -ItemType Directory -Force $GamesDir | Out-Null }

# --- HTTP (UTF-8 both ways; PowerShell 5.1's Invoke-RestMethod guesses the charset) --
function Invoke-Http([string]$method, [string]$url, $body = $null, [hashtable]$headers = @{}, [string]$contentType = 'application/json') {
    $req = [Net.HttpWebRequest]::Create($url)
    $req.Method = $method
    $req.KeepAlive = $false   # the server closes idle connections: a reused one failed a poll
    $req.Timeout = 60000
    $req.ReadWriteTimeout = 60000
    $req.UserAgent = "WECampBroadcaster/$AgentVersion"
    foreach ($k in $headers.Keys) { $req.Headers[$k] = $headers[$k] }
    if ($body -is [byte[]]) {
        $bytes = $body   # a file (the thumbnail): as is
        $req.ContentType = $contentType
        $req.ContentLength = $bytes.Length
        $s = $req.GetRequestStream(); $s.Write($bytes, 0, $bytes.Length); $s.Close()
    } elseif ($null -ne $body) {
        $text = if ($body -is [string]) { $body } else { $body | ConvertTo-Json -Depth 10 -Compress }
        $bytes = [Text.Encoding]::UTF8.GetBytes($text)
        $req.ContentType = "$contentType; charset=utf-8"
        $req.ContentLength = $bytes.Length
        $s = $req.GetRequestStream(); $s.Write($bytes, 0, $bytes.Length); $s.Close()
    } elseif ($method -ne 'GET') { $req.ContentLength = 0 }
    try {
        $resp = $req.GetResponse()
    } catch [Net.WebException] {
        $r = $_.Exception.Response
        if ($null -eq $r) { throw }
        $msg = (New-Object IO.StreamReader($r.GetResponseStream(), [Text.Encoding]::UTF8)).ReadToEnd()
        throw ("HTTP {0} em {1}: {2}" -f [int]$r.StatusCode, ($url -replace '\?.*', ''), $msg.Substring(0, [Math]::Min(400, $msg.Length)))
    }
    $text = (New-Object IO.StreamReader($resp.GetResponseStream(), [Text.Encoding]::UTF8)).ReadToEnd()
    $resp.Close()
    if ($text.Trim()) { return $text | ConvertFrom-Json }
    return $null
}

function Invoke-Site([string]$method, [string]$path, $body = $null) {
    return Invoke-Http $method ($Server + $path) $body @{ 'Authorization' = 'Bearer ' + $Config.token }
}

function ConvertTo-Form([hashtable]$values) {
    return (($values.Keys | ForEach-Object { [Uri]::EscapeDataString($_) + '=' + [Uri]::EscapeDataString([string]$values[$_]) }) -join '&')
}

# --- YouTube (OAuth "installed app" + Data API v3) ---------------------------------
$TokenPath = Join-Path $StateDir 'youtube-token.json'
$StreamPath = Join-Path $StateDir 'youtube-stream.json'
$YT = 'https://www.googleapis.com/youtube/v3'

function Get-Client {
    if (-not $ClientSecretPath -or -not (Test-Path $ClientSecretPath)) { throw 'Credencial do Google nao encontrada (assets\client_secret_*.json).' }
    $j = Get-Content $ClientSecretPath -Raw | ConvertFrom-Json
    if ($j.installed) { return $j.installed } else { return $j.web }
}

function Save-Json($obj, [string]$path) { $obj | ConvertTo-Json -Depth 10 | Set-Content -Path $path -Encoding UTF8 }

function Get-AccessToken {
    if (-not (Test-Path $TokenPath)) { throw 'YouTube nao autorizado - rode autorizar-youtube.cmd numa sessao com navegador.' }
    $t = Get-Content $TokenPath -Raw | ConvertFrom-Json
    $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    if ($t.access_token -and [int64]$t.expires_at -gt ($now + 120)) { return $t.access_token }
    $c = Get-Client
    $r = Invoke-Http 'POST' $c.token_uri (ConvertTo-Form @{ client_id = $c.client_id; client_secret = $c.client_secret;
            refresh_token = $t.refresh_token; grant_type = 'refresh_token' }) @{} 'application/x-www-form-urlencoded'
    $t | Add-Member -Force NoteProperty access_token $r.access_token
    $t | Add-Member -Force NoteProperty expires_at ($now + [int64]$r.expires_in)
    Save-Json $t $TokenPath
    return $t.access_token
}

function Invoke-YouTube([string]$method, [string]$path, $body = $null) {
    return Invoke-Http $method ($YT + $path) $body @{ 'Authorization' = 'Bearer ' + (Get-AccessToken) }
}

function Start-Authorization {
    $c = Get-Client
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    $buf = New-Object byte[] 48; $rng.GetBytes($buf)
    $verifier = [Convert]::ToBase64String($buf).TrimEnd('=').Replace('+', '-').Replace('/', '_')
    $sha = [Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::ASCII.GetBytes($verifier))
    $challenge = [Convert]::ToBase64String($sha).TrimEnd('=').Replace('+', '-').Replace('/', '_')
    # The browser opens http://localhost:<port>: listen on 127.0.0.1 and on ::1 (same port).
    $v4 = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0)
    $v4.Start()
    $port = $v4.LocalEndpoint.Port
    $listeners = @($v4)
    try { $v6 = New-Object Net.Sockets.TcpListener([Net.IPAddress]::IPv6Loopback, $port); $v6.Start(); $listeners += $v6 } catch { }
    $redirect = "http://localhost:$port"
    $url = 'https://accounts.google.com/o/oauth2/v2/auth?' + (ConvertTo-Form @{ client_id = $c.client_id; redirect_uri = $redirect;
            response_type = 'code'; scope = 'https://www.googleapis.com/auth/youtube'; access_type = 'offline'; prompt = 'consent';
            code_challenge = $challenge; code_challenge_method = 'S256' })
    Write-Host "Abrindo o navegador para autorizar o canal do YouTube (porta $port)..."
    Write-Host "Se nao abrir, cole este endereco no navegador DESTA maquina:`n$url`n"
    Start-Process $url
    $deadline = (Get-Date).AddMinutes(5); $code = $null
    while (-not $code -and (Get-Date) -lt $deadline) {
        foreach ($l in $listeners) {
            if (-not $l.Pending()) { continue }
            $client = $l.AcceptTcpClient(); $stream = $client.GetStream()
            $reader = New-Object IO.StreamReader($stream); $first = $reader.ReadLine()
            $ok = $first -match 'code=([^&\s]+)'
            if ($ok) { $code = [Uri]::UnescapeDataString($Matches[1]) }
            $msg = if ($ok) { 'Pronto! O broadcaster do WE Camp foi autorizado. Pode fechar esta aba.' } else { 'Autorizacao recusada ou com erro: ' + $first }
            $html = [Text.Encoding]::UTF8.GetBytes("<html><body style='font-family:sans-serif'><h2>$msg</h2></body></html>")
            $head = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 200 OK`r`nContent-Type: text/html; charset=utf-8`r`nContent-Length: $($html.Length)`r`nConnection: close`r`n`r`n")
            $stream.Write($head, 0, $head.Length); $stream.Write($html, 0, $html.Length); $stream.Flush(); $client.Close()
        }
        Start-Sleep -Milliseconds 200
    }
    $listeners | ForEach-Object { $_.Stop() }
    if (-not $code) { throw 'Nao recebi a autorizacao em 5 minutos.' }
    $r = Invoke-Http 'POST' $c.token_uri (ConvertTo-Form @{ code = $code; client_id = $c.client_id; client_secret = $c.client_secret;
            redirect_uri = $redirect; grant_type = 'authorization_code'; code_verifier = $verifier }) @{} 'application/x-www-form-urlencoded'
    if (-not $r.refresh_token) { throw 'O Google nao devolveu o refresh_token.' }
    Save-Json ([pscustomobject]@{ refresh_token = $r.refresh_token; access_token = $r.access_token;
            expires_at = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + [int64]$r.expires_in }) $TokenPath
    $ch = Invoke-YouTube 'GET' '/channels?part=snippet&mine=true'
    Write-Host ("Autorizado para o canal: {0}" -f $ch.items[0].snippet.title)
}

$StreamsPath = Join-Path $StateDir 'youtube-streams.json'
$script:StreamChecked = @{}

function Get-FreeStream {
    # One reusable YouTube stream per simultaneous live (a stream feeds one live at
    # a time): the first one not used by a running job, or a new one. Their keys
    # never leave this machine.
    $busy = @($script:Jobs.Values | Where-Object { $_.state -in @('preparing', 'running', 'closing') -and $_.stream_id } |
            ForEach-Object { $_.stream_id })
    $saved = $null
    if (Test-Path $StreamsPath) { $saved = Get-Content $StreamsPath -Raw | ConvertFrom-Json }
    elseif (Test-Path $StreamPath) { $saved = Get-Content $StreamPath -Raw | ConvertFrom-Json }  # old single-stream file
    $keep = @(); $free = $null
    foreach ($s in $saved) {
        if (-not $s.id) { continue }
        if (-not $script:StreamChecked[$s.id]) {
            $found = Invoke-YouTube 'GET' "/liveStreams?part=id&id=$($s.id)"
            if ($found.items.Count -eq 0) { Write-Log "Stream do YouTube $($s.id) sumiu."; continue }
            $script:StreamChecked[$s.id] = $true
        }
        $keep += $s
        if (-not $free -and $busy -notcontains $s.id) { $free = $s }
    }
    if (-not $free) {
        $n = $keep.Count + 1
        $r = Invoke-YouTube 'POST' '/liveStreams?part=snippet,cdn,contentDetails' @{
            snippet = @{ title = "WE Camp broadcaster ($($Config.name)) $n" }
            cdn = @{ ingestionType = 'rtmp'; resolution = 'variable'; frameRate = 'variable' }
            contentDetails = @{ isReusable = $true } }
        $free = [pscustomobject]@{ id = $r.id; key = $r.cdn.ingestionInfo.streamName; address = $r.cdn.ingestionInfo.ingestionAddress }
        $script:StreamChecked[$free.id] = $true
        $keep += $free
        Write-Log "Stream do YouTube criado: $($free.id) ($n)"
    }
    ConvertTo-Json -InputObject @($keep) -Depth 5 | Set-Content -Path $StreamsPath -Encoding UTF8
    return $free
}

function New-Broadcast($job) {
    $stream = Get-FreeStream
    $b = Invoke-YouTube 'POST' '/liveBroadcasts?part=snippet,status,contentDetails' @{
        snippet = @{ title = $job.title; description = $job.description; scheduledStartTime = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ') }
        status = @{ privacyStatus = $job.privacy; selfDeclaredMadeForKids = $false }
        contentDetails = @{ enableAutoStart = $true; enableAutoStop = $true; enableDvr = $true; recordFromStart = $true
            latencyPreference = 'normal'; monitorStream = @{ enableMonitorStream = $false } } }
    Invoke-YouTube 'POST' "/liveBroadcasts/bind?part=id&id=$($b.id)&streamId=$($stream.id)" | Out-Null
    return [pscustomobject]@{ id = $b.id; url = "https://www.youtube.com/watch?v=$($b.id)"; stream_id = $stream.id
        rtmp = ($stream.address.TrimEnd('/') + '/' + $stream.key) }
}

function Set-Thumbnail([string]$id) {
    # The live's thumbnail (shown before it starts, in the channel's list and in shared links).
    # Needs a channel allowed to use custom thumbnails; otherwise just a warning.
    $img = @('miniatura.jpg', 'miniatura.png') | ForEach-Object { Join-Path $MediaDir $_ } |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $img) { return }
    $type = if ($img -like '*.png') { 'image/png' } else { 'image/jpeg' }
    try {
        Invoke-Http 'POST' "https://www.googleapis.com/upload/youtube/v3/thumbnails/set?videoId=$id" ([IO.File]::ReadAllBytes($img)) `
            @{ 'Authorization' = 'Bearer ' + (Get-AccessToken) } $type | Out-Null
    } catch { Write-Log "Aviso: miniatura nao enviada para a live ${id}: $($_.Exception.Message)" }
}

function Complete-Broadcast([string]$id, [bool]$started) {
    if (-not $id) { return }
    try {
        $b = Invoke-YouTube 'GET' "/liveBroadcasts?part=status&id=$id"
        if ($b.items.Count -eq 0) { return }
        $life = $b.items[0].status.lifeCycleStatus
        if ($life -in 'live', 'liveStarting', 'testing') {
            Invoke-YouTube 'POST' "/liveBroadcasts/transition?broadcastStatus=complete&part=status&id=$id" | Out-Null
            Write-Log "Live $id encerrada no YouTube."
        } elseif (-not $started -and $life -in 'created', 'ready') {
            Invoke-YouTube 'DELETE' "/liveBroadcasts?id=$id" | Out-Null  # nunca transmitiu: nao fica uma live vazia no canal
            Write-Log "Live $id (sem transmissao) apagada do YouTube."
        }
    } catch { Write-Log "Aviso: nao consegui encerrar a live ${id}: $($_.Exception.Message)" }
}

# --- videos around the game: "aguarde" before it, "fim" / "interrompida" after it ---
# midia\<name>.mp4 (any size/format) is re-encoded once into midia\cache\<name>.flv the
# way record_wecamp sends the game (640x480, 30 fps, H.264 with a 2 s GOP, AAC 44.1 kHz
# stereo), so it goes out with -c copy and YouTube sees one consistent stream. The end
# videos get $ClipHold s of their last frame, so ending the live doesn't cut them.
$ClipSize = '640x480'; $ClipFps = 30; $ClipVBitrate = '1500k'; $ClipBufsize = '3000k'; $ClipABitrate = '128k'
$ClipRate = 44100; $ClipHold = 5

function Get-Clip([string]$name) {
    $src = Join-Path $MediaDir "$name.mp4"
    if (-not (Test-Path -LiteralPath $src)) { return $null }
    $cacheDir = Join-Path $MediaDir 'cache'
    if (-not (Test-Path $cacheDir)) { New-Item -ItemType Directory -Force $cacheDir | Out-Null }
    $out = Join-Path $cacheDir "$name.flv"
    $stampPath = Join-Path $cacheDir "$name.src"
    $f = Get-Item -LiteralPath $src
    $stamp = '{0}|{1}' -f $f.Length, $f.LastWriteTimeUtc.Ticks   # a replaced video (even with an older date) is redone
    if ((Test-Path -LiteralPath $out) -and (Test-Path -LiteralPath $stampPath) -and
            ((Get-Content -LiteralPath $stampPath -Raw).Trim() -eq $stamp)) { return $out }
    $w, $h = $ClipSize -split 'x'
    $vf = "scale=${w}:${h}:force_original_aspect_ratio=decrease,pad=${w}:${h}:(ow-iw)/2:(oh-ih)/2,setsar=1,fps=$ClipFps,format=yuv420p"
    $af = "aresample=$ClipRate"
    if ($name -ne 'aguarde') { $vf += ",tpad=stop_mode=clone:stop_duration=$ClipHold"; $af += ",apad=pad_dur=$ClipHold" }
    # 'Continue' here: under 'Stop', Windows PowerShell turns any stderr line of a native
    # command into an error, and the video would go out with silence instead of its sound.
    $ErrorActionPreference = 'Continue'
    $hasAudio = [bool](& $FfprobeExe -v error -select_streams a -show_entries stream=index -of csv=p=0 $src 2>$null)
    $ErrorActionPreference = 'Stop'
    $tmp = Join-Path $cacheDir "$name.tmp.flv"
    $a = @('-y', '-hide_banner', '-nostdin', '-loglevel', 'error', '-i', "`"$src`"")
    if ($hasAudio) { $a += @('-map', '0:v:0', '-map', '0:a:0') }
    else { $a += @('-f', 'lavfi', '-i', "anullsrc=r=${ClipRate}:cl=stereo", '-map', '0:v:0', '-map', '1:a:0', '-shortest') }
    $a += @('-vf', "`"$vf`"", '-af', "`"$af`"", '-c:v', 'libx264', '-preset', 'veryfast', '-b:v', $ClipVBitrate,
        '-maxrate', $ClipVBitrate, '-bufsize', $ClipBufsize, '-g', [string]($ClipFps * 2), '-keyint_min', [string]($ClipFps * 2),
        '-sc_threshold', '0', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', $ClipABitrate, '-ar', [string]$ClipRate, '-ac', '2',
        '-f', 'flv', "`"$tmp`"")
    $log = Join-Path $LogDir "video-$name.log"
    $p = Start-Process -FilePath $FfmpegExe -ArgumentList $a -PassThru -NoNewWindow -RedirectStandardError $log
    $null = $p.Handle
    if (-not $p.WaitForExit(180000)) { try { $p.Kill() } catch { }; throw "o preparo do video $name passou de 3 min" }
    if ($p.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $tmp)) { throw "nao consegui preparar o video $name (ffmpeg $($p.ExitCode), veja $log)" }
    Move-Item -LiteralPath $tmp -Destination $out -Force
    Set-Content -LiteralPath $stampPath -Value $stamp -Encoding ASCII
    Write-Log "Video $name preparado para a live."
    return $out
}

function Start-Clip($job, [string]$name, [switch]$Loop) {
    # Sends a clip to the job's YouTube stream at its own pace (-re); returns the process.
    $clip = Get-Clip $name
    if (-not $clip) { Write-Log "Aviso: falta o video $name.mp4 em $MediaDir."; return $null }
    $a = @('-hide_banner', '-nostdin', '-loglevel', 'warning', '-re')
    if ($Loop) { $a += @('-stream_loop', '-1') }
    $a += @('-i', "`"$clip`"", '-c', 'copy')
    if ($Loop) { $a += @('-t', [string][int]($IntroMaxMinutes * 60)) }
    $a += @('-f', 'flv', '-flvflags', 'no_duration_filesize', "`"$($job.rtmp)`"")
    $log = Join-Path $LogDir "video-$name-job-$($job.id).log"
    $p = Start-Process -FilePath $FfmpegExe -ArgumentList $a -PassThru -NoNewWindow -RedirectStandardError $log
    $null = $p.Handle   # keeps the handle, or ExitCode stays empty
    return $p
}

function Stop-Intro($job) {
    # The "Aguarde" video: ffmpeg-jogo.cmd normally stopped it when the game went out
    # (and deleted the PID file); otherwise (RetroArch failed before that) it stops here.
    if ($job.intro -and -not $job.intro.HasExited) { try { $job.intro.Kill() } catch { } }
    Remove-Item -LiteralPath $job.intro_pidfile -Force -ErrorAction SilentlyContinue
}

# --- ISOs -------------------------------------------------------------------------
$IndexPath = Join-Path $StateDir 'isos.json'
$DownloadDir = Join-Path $GamesDir '.baixando'
$script:Index = @{}      # md5 -> @{ path; ref; size; mtime; aliases = @() }
$script:Download = $null # @{ md5; url; file; ref; size; part; proc; started }
$script:Wanted = @()     # registry ISOs waiting to be downloaded (queue)
$script:RetryAt = @{}    # md5 -> not before (a failed download, resumed later)
$script:Urgent = @{}     # md5 -> a match is waiting for it (goes first, pauses a background one)

function Save-Index { Save-Json $script:Index $IndexPath }

function Load-Index {
    $script:Index = @{}
    if (Test-Path $IndexPath) {
        $j = Get-Content $IndexPath -Raw | ConvertFrom-Json
        foreach ($p in $j.PSObject.Properties) {
            $v = $p.Value
            $script:Index[$p.Name] = @{ path = $v.path; ref = $v.ref; size = [int64]$v.size; mtime = [string]$v.mtime; aliases = @($v.aliases | Where-Object { $_ }) }
        }
    }
}

function Update-Index {
    # Files placed by hand: hash once (size+date unchanged = same file).
    $changed = $false
    foreach ($md5 in @($script:Index.Keys)) {
        $e = $script:Index[$md5]
        if (-not (Test-Path -LiteralPath $e.path)) { $script:Index.Remove($md5); $changed = $true }
    }
    $known = @{}
    foreach ($e in $script:Index.Values) { $known[$e.path.ToLower()] = $true; foreach ($a in $e.aliases) { $known[$a.ToLower()] = $true } }
    foreach ($f in Get-ChildItem -LiteralPath $GamesDir -File -ErrorAction SilentlyContinue) {
        if ($f.Extension -notin '.bin', '.chd', '.iso', '.img', '.pbp') { continue }
        if ($known[$f.FullName.ToLower()]) { continue }
        $md5 = (Get-FileHash -LiteralPath $f.FullName -Algorithm MD5).Hash.ToLower()
        if ($script:Index[$md5]) { $script:Index[$md5].aliases += $f.FullName }
        else { $script:Index[$md5] = @{ path = $f.FullName; ref = ''; size = $f.Length; mtime = $f.LastWriteTimeUtc.ToString('o'); aliases = @() } }
        Write-Log "ISO encontrada em games: $($f.Name) ($md5)"
        $changed = $true
    }
    if ($changed) { Save-Index }
}

function Get-FreeGB { return [Math]::Round(([IO.DriveInfo]::new((Split-Path $GamesDir -Qualifier))).AvailableFreeSpace / 1GB, 1) }

function Request-Iso($iso, [bool]$urgent) {
    if (-not $iso -or $script:Index[$iso.md5]) { return }
    if ($script:Download -and $script:Download.md5 -eq $iso.md5) { return }
    $script:Wanted = @($script:Wanted | Where-Object { $_.md5 -ne $iso.md5 })
    if ($urgent) {
        $script:Urgent[$iso.md5] = $true
        $script:RetryAt.Remove($iso.md5)
        $script:Wanted = @($iso) + $script:Wanted
        if ($script:Download -and -not $script:Download.urgent -and -not $script:Download.paused) {
            # A match needs another ISO: the background one pauses (resumed later with -C -).
            $script:Download.paused = $true
            try { $script:Download.proc.Kill() } catch { }
        }
    } else { $script:Wanted += $iso }
}

function Complete-Download($d) {
    $script:Urgent.Remove($d.md5)
    $md5 = (Get-FileHash -LiteralPath $d.part -Algorithm MD5).Hash.ToLower()
    if ($md5 -ne $d.md5) { Remove-Item -LiteralPath $d.part -Force; Write-Log "Download de $($d.file) com MD5 errado ($md5) - descartado."; return }
    $target = Join-Path $GamesDir $d.file
    # Older version of the same registry ISO (same ref): it is replaced, with its aliases.
    foreach ($old in @($script:Index.Keys)) {
        $e = $script:Index[$old]
        if (($d.ref -and $e.ref -eq $d.ref) -or $e.path -ieq $target) {
            foreach ($p in @($e.path) + $e.aliases) { Remove-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue }
            $script:Index.Remove($old)
            Write-Log "ISO $($d.file): versao antiga ($old) substituida."
        }
    }
    Move-Item -LiteralPath $d.part -Destination $target -Force
    $f = Get-Item -LiteralPath $target
    $script:Index[$md5] = @{ path = $target; ref = $d.ref; size = $f.Length; mtime = $f.LastWriteTimeUtc.ToString('o'); aliases = @() }
    Save-Index
    Write-Log "ISO baixada: $($d.file) ($md5)"
}

function Step-Downloads {
    $d = $script:Download
    if ($d) {
        if (-not $d.proc.HasExited) {
            if (((Get-Date) - $d.started).TotalHours -gt 2) { try { $d.proc.Kill() } catch { }; Write-Log "Download de $($d.file) passou de 2 h: cancelado." }
            return
        }
        $script:Download = $null
        if ($d.paused) {
            $script:Wanted += $d.iso
            Write-Log "Download de $($d.file) pausado para baixar a ISO de uma partida."
        } elseif ($d.proc.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $d.part)) {
            # archive.org drops connections now and then: back to the queue, resumed (-C -) in 2 min.
            $script:RetryAt[$d.md5] = (Get-Date).AddMinutes(2)
            $script:Wanted += $d.iso
            Write-Log "Download de $($d.file) falhou (curl $($d.proc.ExitCode)) - continuo em 2 min."
        } else {
            Complete-Download $d
        }
    }
    $later = @()
    while ($script:Wanted.Count -gt 0 -and -not $script:Download) {
        $iso = $script:Wanted[0]; $script:Wanted = @($script:Wanted | Select-Object -Skip 1)
        if ($script:Index[$iso.md5]) { continue }
        if ($script:RetryAt[$iso.md5] -and (Get-Date) -lt $script:RetryAt[$iso.md5]) { $later += $iso; continue }
        if ((Get-FreeGB) - ([double]$iso.size / 1GB) -lt $MinFreeGB) { Write-Log "Sem espaco para baixar $($iso.file) (min. $MinFreeGB GB livres)."; continue }
        if (-not (Test-Path $DownloadDir)) { New-Item -ItemType Directory -Force $DownloadDir | Out-Null }
        $part = Join-Path $DownloadDir ($iso.md5 + '.part')
        $curlArgs = @('-L', '--fail', '--silent', '--show-error', '--retry', '5', '--retry-delay', '15', '--retry-all-errors',
            '-C', '-', '-o', "`"$part`"", "`"$($iso.url)`"")
        $proc = Start-Process -FilePath 'curl.exe' -ArgumentList $curlArgs -PassThru -WindowStyle Hidden
        $null = $proc.Handle  # keeps the handle, or ExitCode stays empty
        $script:Download = @{ md5 = $iso.md5; url = $iso.url; file = $iso.file; ref = $iso.ref; part = $part; proc = $proc;
                              started = Get-Date; iso = $iso; urgent = [bool]$script:Urgent[$iso.md5]; paused = $false }
        Write-Log "Baixando a ISO $($iso.file) ($([Math]::Round([double]$iso.size / 1MB)) MB)."
    }
    if ($later.Count) { $script:Wanted += $later }
}

function Sync-Registry {
    $r = Invoke-Site 'GET' '/api/broadcaster/isos'
    $refs = @{}
    foreach ($e in $script:Index.Values) { if ($e.ref) { $refs[$e.ref] = $true } }
    foreach ($iso in $r.isos) {
        $have = $script:Index[$iso.md5]
        if ($have) {
            if ($have.ref -ne $iso.ref) { $have.ref = $iso.ref; Save-Index }
            continue
        }
        if ($refs[$iso.ref]) { Write-Log "ISO atualizada no cadastro: $($iso.name) - baixando a nova."; Request-Iso $iso $false }
        elseif ($SyncAll) { Request-Iso $iso $false }
    }
}

function Resolve-Game($job) {
    # The file RetroArch must open has the room's name (the Kaillera game name is
    # "PCSX ReARMed: <file>"). Same content under another name: a hard link.
    $target = Join-Path $GamesDir $job.game_file
    $iso = $job.iso
    if (-not $iso) {
        if (Test-Path -LiteralPath $target) { return $target }
        throw "ISO da sala nao esta no cadastro nem em games: $($job.game_file)"
    }
    $e = $script:Index[$iso.md5]
    if (-not $e) { Request-Iso $iso $true; return $null }   # baixando
    if ($e.path -ieq $target) { return $target }
    if ($e.aliases | Where-Object { $_ -ieq $target }) { if (Test-Path -LiteralPath $target) { return $target } }
    if (Test-Path -LiteralPath $target) {
        # A different file with the room's name (another version): keep it aside.
        Move-Item -LiteralPath $target -Destination ($target + '.outra-versao') -Force
    }
    New-Item -ItemType HardLink -Path $target -Target $e.path | Out-Null
    $e.aliases = @($e.aliases) + $target
    Save-Index
    Write-Log "ISO $($job.game_file) ligada a $(Split-Path $e.path -Leaf)."
    return $target
}

# --- jobs -------------------------------------------------------------------------
$script:Jobs = @{}   # id -> job state

function Get-LogTail([string]$path, [int]$lines = 15) {
    if (-not (Test-Path -LiteralPath $path)) { return '' }
    return ((Get-Content -LiteralPath $path -Tail 400 -ErrorAction SilentlyContinue | Where-Object { $_ -match 'WE Camp|ERROR|Kaillera' } |
            Select-Object -Last $lines) -join "`n")
}

function Start-Job2($a) {
    $id = [int]$a.id
    if ($script:Jobs[$id]) { return }
    $job = @{ id = $id; info = $a; state = 'preparing'; youtube_id = $null; youtube_url = $null; proc = $null; error = '';
              created = Get-Date; started = $null; streamed = $false; reported_final = 0
              intro = $null; onair = $false; outro = $null; ending = $null; final_state = $null; closing_since = $null }
    # ending: the end video the site chose ("fim" / "interrompida"); empty = decided when RetroArch quits.
    if ($a.ending) { $job.ending = [string]$a.ending }
    $job.control = Join-Path $RunDir "job-$id.ctl"
    $job.intro_pidfile = Join-Path $RunDir "job-$id.intro"
    $job.log = Join-Path $LogDir "retroarch-job-$id.log"
    Remove-Item -LiteralPath $job.control, $job.intro_pidfile -Force -ErrorAction SilentlyContinue
    if ($null -ne $a.stop_frame) {
        # Reprise (partida encerrada): o fim ja vem no inicio.
        Set-Content -LiteralPath $job.control -Value "stop_frame=$($a.stop_frame)" -Encoding ASCII
        $job.stop_frame = [int]$a.stop_frame
    }
    $script:Jobs[$id] = $job
    Write-Log "Trabalho ${id}: sessao $($a.session_id), $($a.title)"
}

function Step-Job($job) {
    if ($job.state -eq 'preparing') {
        try {
            $game = Resolve-Game $job.info
            if (-not $game) { return }   # waiting for the ISO
            if (-not $job.youtube_id) {
                $b = New-Broadcast $job.info
                $job.youtube_id = $b.id; $job.youtube_url = $b.url; $job.rtmp = $b.rtmp; $job.stream_id = $b.stream_id
                Write-Log "Trabalho $($job.id): live criada $($b.url)"
                Set-Thumbnail $b.id
            }
            # "Aguarde" until the game goes out (the catch-up to the kickoff takes a while). Only
            # with ffmpeg-jogo.cmd, which stops it before RetroArch's ffmpeg sends the game.
            $useWrapper = Test-Path -LiteralPath $GameFfmpeg
            if ($useWrapper -and -not $job.intro) {
                try { $job.intro = Start-Clip $job 'aguarde' -Loop }
                catch { Write-Log "Aviso: trabalho $($job.id) sem o video de espera: $($_.Exception.Message)" }
                if ($job.intro) {
                    Set-Content -LiteralPath $job.intro_pidfile -Value $job.intro.Id -Encoding ASCII
                    $job.onair = $true
                    Write-Log "Trabalho $($job.id): video de espera no ar."
                }
            }
            $env:WECAMP_BROADCAST_FROM_FRAME = [string]$job.info.start_frame
            # The card's savestate from the host's PC (site 2026-11-22): RetroArch loads it and
            # streams from there at once - no fast-forward (0 = none: fast-forward as before).
            $checkpoint = 0
            if ($job.info.checkpoint) { $checkpoint = [int]$job.info.checkpoint }
            $env:WECAMP_BROADCAST_CHECKPOINT = [string]$checkpoint
            $env:WECAMP_CONTROL_FILE = $job.control
            $env:WECAMP_LOG_NAME = "job-$($job.id)"
            if ($useWrapper) {
                $env:WECAMP_FFMPEG = $GameFfmpeg
                $env:WECAMP_FFMPEG_REAL = $FfmpegExe
                $env:WECAMP_INTRO_PIDFILE = $job.intro_pidfile
            }
            try {
                $rel = 'games\' + (Split-Path $game -Leaf)
                $job.proc = Start-Process -FilePath 'cmd.exe' -WorkingDirectory $Root -WindowStyle Hidden -PassThru `
                    -ArgumentList '/c', 'start-broadcast.cmd', $job.info.session_id, "`"$rel`"", $job.rtmp
            } finally {
                Remove-Item Env:WECAMP_BROADCAST_FROM_FRAME, Env:WECAMP_BROADCAST_CHECKPOINT, Env:WECAMP_CONTROL_FILE, Env:WECAMP_LOG_NAME, Env:WECAMP_FFMPEG,
                    Env:WECAMP_FFMPEG_REAL, Env:WECAMP_INTRO_PIDFILE -ErrorAction SilentlyContinue
            }
            $job.state = 'running'; $job.started = Get-Date
            Write-Log "Trabalho $($job.id): broadcaster iniciado (a partir do quadro $($job.info.start_frame)$(if ($checkpoint) { ', pelo state save do host' }))."
        } catch {
            $job.state = 'failed'; $job.error = $_.Exception.Message
            Write-Log "Trabalho $($job.id) falhou ao preparar: $($job.error)"
            Stop-Intro $job
            Complete-Broadcast $job.youtube_id $job.onair
        }
        return
    }
    if ($job.state -eq 'closing') {
        # The end video is on: when it is over (plus a margin for the viewers' delay), end the live.
        $done = $job.outro.HasExited
        $age = ((Get-Date) - $job.closing_since).TotalSeconds
        if (-not $done -and $age -lt 90) { return }
        if (-not $done) { try { $job.outro.Kill() } catch { } }
        if (-not $job.outro_done) { $job.outro_done = Get-Date }
        if (((Get-Date) - $job.outro_done).TotalSeconds -lt $EndDelaySeconds) { return }
        Complete-Broadcast $job.youtube_id $true
        $job.state = $job.final_state
        Write-Log "Trabalho $($job.id): $($job.state)."
        return
    }
    if ($job.state -eq 'running') {
        if (-not $job.streamed -and (Test-Path -LiteralPath $job.log)) {
            if (Select-String -LiteralPath $job.log -Pattern '\[WE Camp\]: streaming' -Quiet) { $job.streamed = $true; Write-Log "Trabalho $($job.id): transmitindo." }
        }
        if (((Get-Date) - $job.started).TotalHours -gt $MaxJobHours) {
            Write-Log "Trabalho $($job.id) passou de $MaxJobHours h: encerrando."
            Set-Content -LiteralPath $job.control -Value 'stop_frame=0' -Encoding ASCII
            if (((Get-Date) - $job.started).TotalHours -gt $MaxJobHours + 0.1) { Stop-Tree $job.proc }
        }
        if ($job.proc.HasExited) {
            Stop-Intro $job
            $tail = Get-LogTail $job.log
            $final = 'ended'
            if ($tail -notmatch 'stream finished|session ended|reached stop frame') {
                $final = 'failed'; $job.error = 'O RetroArch fechou sem terminar a transmissao.'
            }
            $job.tail = $tail
            # The end video: the site's choice; without it, "fim" only when the match ran to the
            # stop frame (or a replay to its end) - the host's session ending first = "interrompida".
            $ending = $job.ending
            if ($ending -notin 'fim', 'interrompida') {
                $ending = if ($final -eq 'ended' -and ($tail -match 'reached stop frame' -or $job.info.replay)) { 'fim' } else { 'interrompida' }
            }
            if ($job.onair -or $job.streamed) {
                try { $job.outro = Start-Clip $job $ending }
                catch { Write-Log "Aviso: trabalho $($job.id) sem o video de encerramento: $($_.Exception.Message)" }
                if ($job.outro) {
                    $job.final_state = $final; $job.state = 'closing'; $job.closing_since = Get-Date
                    Write-Log "Trabalho $($job.id): video de encerramento ($ending) no ar."
                    return
                }
            }
            $job.state = $final
            Complete-Broadcast $job.youtube_id ($job.streamed -or $job.onair)
            Write-Log "Trabalho $($job.id): $($job.state)."
        }
    }
}

function Stop-Tree($proc) {
    if (-not $proc) { return }
    try { & taskkill.exe /PID $proc.Id /T /F | Out-Null } catch { }
}

function Stop-Job2($a) {
    $job = $script:Jobs[[int]$a.id]
    if (-not $job) { return }
    $frame = [int]$a.stop_frame
    if ($a.ending -and $job.ending -ne [string]$a.ending) {
        $job.ending = [string]$a.ending   # the latest word from the site counts (when RetroArch quits)
        Write-Log "Trabalho $($job.id): video de encerramento = $($job.ending)."
    }
    if ($job.state -eq 'preparing' -and $frame -eq 0) {
        # Quadro 0 = cancelada (pelo administrador, ou deu a vaga a uma partida ao vivo).
        $job.state = 'ended'; $job.error = 'Parada antes de comecar.'
        Complete-Broadcast $job.youtube_id $false
        Write-Log "Trabalho $($job.id): parado antes de comecar."
        return
    }
    if (($job.state -in @('preparing', 'running')) -and ($job.stop_frame -ne $frame)) {
        # Preparando: o RetroArch le o arquivo quando abrir.
        Set-Content -LiteralPath $job.control -Value "stop_frame=$frame" -Encoding ASCII
        $job.stop_frame = $frame
        Write-Log "Trabalho $($job.id): parar no quadro $frame."
    }
}

function Get-Reports {
    $list = @()
    foreach ($job in $script:Jobs.Values) {
        $state = switch ($job.state) {
            'preparing' { 'starting' }
            'running' { if ($job.streamed -or $job.onair) { 'live' } else { 'starting' } }
            'closing' { 'live' }   # the end video is still on
            default { $job.state } }
        $err = $job.error
        if ($job.state -eq 'failed' -and $job.tail) { $err = $err + "`n" + $job.tail }
        $list += @{ id = $job.id; state = $state; youtube_id = $job.youtube_id; youtube_url = $job.youtube_url; error = $err }
    }
    return , $list
}

function Get-Status {
    $cpu = $null
    try { $cpu = [int](Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average } catch { }
    $yt = if (Test-Path $TokenPath) { 'autorizado' } else { 'nao autorizado' }
    $dl = if ($script:Download) { $script:Download.file } else { '' }
    return @{ cpu = $cpu; youtube = $yt; isos = $script:Index.Count; downloading = $dl; disk_free_gb = (Get-FreeGB);
              waiting_isos = $script:Wanted.Count }
}

# --- main -------------------------------------------------------------------------
# Dot-sourced (test-videos.ps1): just the functions above.
if ($MyInvocation.InvocationName -eq '.') { return }

if ($Authorize) { Start-Authorization; exit 0 }

Load-Index
Update-Index

if ($Check) {
    try {
        $r = Invoke-Site 'POST' '/api/broadcaster/poll' @{ version = $AgentVersion; slots = 0; status = (Get-Status); jobs = @() }
        Write-Host "Site OK: $Server (consulta a cada $($r.poll_seconds) s)."
    } catch { Write-Host "Site: ERRO - $($_.Exception.Message)" }
    try {
        $ch = Invoke-YouTube 'GET' '/channels?part=snippet&mine=true'
        Write-Host ("YouTube OK: canal {0}" -f $ch.items[0].snippet.title)
        $s = Get-FreeStream
        Write-Host "Stream do YouTube livre deste broadcaster: $($s.id) (um por live simultanea; $Slots vaga(s))"
    } catch { Write-Host "YouTube: ERRO - $($_.Exception.Message)" }
    Write-Host ("ISOs em games: {0}; {1} GB livres" -f $script:Index.Count, (Get-FreeGB))
    exit 0
}

try { $mutex = New-Object Threading.Mutex($false, 'Global\WECampBroadcasterAgent') }
catch { $mutex = New-Object Threading.Mutex($false, 'Local\WECampBroadcasterAgent') }  # sem permissao para o Global
if (-not $mutex.WaitOne(0)) { Write-Log 'Outro agente ja esta rodando.'; exit 1 }
Write-Log "Agente $AgentVersion iniciado ($Server, $Slots vaga(s))."
# An "Aguarde" video left on the air by an agent that died (it would keep the YouTube stream busy).
foreach ($f in Get-ChildItem -LiteralPath $RunDir -Filter 'job-*.intro' -ErrorAction SilentlyContinue) {
    $introId = [int]((Get-Content -LiteralPath $f.FullName -Raw) -replace '\D', '')
    Get-Process -Id $introId -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -eq 'ffmpeg' } | Stop-Process -Force
    Remove-Item -LiteralPath $f.FullName -Force -ErrorAction SilentlyContinue
}
# The videos ready now (re-encoded once, a few seconds each), not when a live starts.
foreach ($name in 'aguarde', 'fim', 'interrompida') {
    try { $null = Get-Clip $name } catch { Write-Log "Aviso: video $name - $($_.Exception.Message)" }
}
$lastSync = [DateTime]::MinValue
$pollSeconds = 5
# Updated agent.ps1 or config.json: exit when idle - the scheduled task starts it again
# (every 5 min), with the new code/config.
$selfStamp = (Get-Item -LiteralPath $PSCommandPath).LastWriteTimeUtc
$cfgStamp = (Get-Item -LiteralPath $ConfigPath).LastWriteTimeUtc
while ($true) {
    $busy = @($script:Jobs.Values | Where-Object { $_.state -in @('preparing', 'running', 'closing') }).Count
    if (-not $busy -and -not $script:Download -and (((Get-Item -LiteralPath $PSCommandPath).LastWriteTimeUtc -ne $selfStamp) -or
            ((Get-Item -LiteralPath $ConfigPath).LastWriteTimeUtc -ne $cfgStamp))) {
        Write-Log 'agent.ps1 ou config.json mudou: saindo para a tarefa agendada abrir de novo.'
        break
    }
    try {
        foreach ($job in @($script:Jobs.Values)) { Step-Job $job }
        Step-Downloads
        $r = Invoke-Site 'POST' '/api/broadcaster/poll' @{ version = $AgentVersion; slots = $Slots; status = (Get-Status); jobs = (Get-Reports) }
        if ($r.poll_seconds) { $pollSeconds = [int]$r.poll_seconds }
        # Final states reported: forget them (twice, in case a reply got lost).
        foreach ($job in @($script:Jobs.Values)) {
            if ($job.state -in 'ended', 'failed') { $job.reported_final++; if ($job.reported_final -ge 2) { $script:Jobs.Remove($job.id) } }
        }
        foreach ($a in $r.jobs) {
            if ($a.action -eq 'start') { Start-Job2 $a } elseif ($a.action -eq 'stop') { Stop-Job2 $a }
        }
        foreach ($job in @($script:Jobs.Values)) { if ($job.state -eq 'preparing') { Step-Job $job } }
        if (((Get-Date) - $lastSync).TotalMinutes -ge 60) {
            $lastSync = Get-Date
            Update-Index
            Sync-Registry
        }
    } catch {
        Write-Log "Erro: $($_.Exception.Message)"
    }
    if ($Once) { break }
    # Between polls, react fast when a RetroArch quits or an end video is over: the end
    # video should follow the game with no gap on YouTube.
    $nextPoll = (Get-Date).AddSeconds($pollSeconds)
    while ((Get-Date) -lt $nextPoll) {
        Start-Sleep -Milliseconds 250
        foreach ($job in @($script:Jobs.Values)) {
            if (($job.state -eq 'running' -and $job.proc.HasExited) -or $job.state -eq 'closing') {
                try { Step-Job $job } catch { Write-Log "Erro: $($_.Exception.Message)" }
            }
        }
    }
}
