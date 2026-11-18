param([string]$Session = '36368-1794977438', [string]$Game = 'games\WE PW26 INTERNATIONAL - WE2002.chd',
      [int]$Seconds = 300)
# Live test to YouTube: streams a session for $Seconds to rtmp://a.rtmp.youtube.com/live2/<ChaveYoutube>,
# then stops. The stream key comes from the machine's ChaveYoutube variable and is masked in all output.
Set-Location C:\broadcaster
if (-not $env:ChaveYoutube) { "ChaveYoutube nao definida"; exit 1 }
function Hide-Key([string]$s) { if ($s) { $s.Replace($env:ChaveYoutube, '<chave>') } }
$url = "rtmp://a.rtmp.youtube.com/live2/$env:ChaveYoutube"
Remove-Item "logs\retroarch-$Session.log", "logs\ffmpeg-$Session.log" -EA SilentlyContinue
$p = Start-Process cmd -ArgumentList "/c start-broadcast.cmd $Session `"$Game`" $url" -PassThru -WindowStyle Hidden
$ncpu = [Environment]::ProcessorCount; $prev = @{}
for ($t = 15; $t -le $Seconds; $t += 15) {
    Start-Sleep 15
    $tot = 0
    foreach ($pr in Get-Process retroarch, ffmpeg -EA SilentlyContinue) {
        $d = $pr.CPU - [double]$prev[$pr.Id]; $prev[$pr.Id] = $pr.CPU; $tot += 100 * $d / 15 / $ncpu
    }
    $stat = ''
    $log = Get-Content "logs\ffmpeg-$Session.log" -Raw -EA SilentlyContinue
    if ($log) { $stat = (($log -split "[\r\n]+") | ? { $_ -match 'frame=' } | Select -Last 1) -replace '\s+', ' ' }
    "t={0,3}s CPU={1,3:N0}% {2}" -f $t, $tot, (Hide-Key $stat)
    if ($p.HasExited) { "RetroArch saiu sozinho"; break }
}
Get-Process retroarch -EA SilentlyContinue | Stop-Process -Force
Start-Sleep 8
"== ffmpeg ainda vivo? " + [bool](Get-Process ffmpeg -EA SilentlyContinue)
"== RetroArch log ([WE Camp]):"
Get-Content "logs\retroarch-$Session.log" -EA SilentlyContinue | Select-String 'WE Camp' | % { Hide-Key $_.Line }
"== ffmpeg log (avisos/erros):"
$log = Get-Content "logs\ffmpeg-$Session.log" -Raw -EA SilentlyContinue
if ($log) { ($log -split "[\r\n]+") | ? { $_ -ne '' -and $_ -notmatch '^frame=' } | Select -Last 15 | % { Hide-Key $_ } }
