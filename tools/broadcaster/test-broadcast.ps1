param([string]$Session = '36368-1794977438', [string]$Game = 'games\WE PW26 INTERNATIONAL - WE2002.chd',
      [string]$Out = 'C:\broadcaster\out\teste1.flv', [int]$Seconds = 100)
# Smoke test: runs start-broadcast.cmd for $Seconds, samples CPU, then stops RetroArch and reports.
Set-Location C:\broadcaster
New-Item -ItemType Directory -Force out | Out-Null
Remove-Item $Out, "logs\retroarch-$Session.log", "logs\ffmpeg-$Session.log" -EA SilentlyContinue
$p = Start-Process cmd -ArgumentList "/c start-broadcast.cmd $Session `"$Game`" $Out" -PassThru -WindowStyle Hidden
$ncpu = [Environment]::ProcessorCount; $prev = @{}
for ($t = 10; $t -le $Seconds; $t += 10) {
    Start-Sleep 10
    $tot = 0; $parts = @()
    foreach ($pr in Get-Process retroarch, ffmpeg -EA SilentlyContinue) {
        $d = $pr.CPU - [double]$prev[$pr.Id]; $prev[$pr.Id] = $pr.CPU
        $u = 100 * $d / 10 / $ncpu; $tot += $u; $parts += ("{0}={1:N0}%" -f $pr.Name, $u)
    }
    $size = if (Test-Path $Out) { [math]::Round((Get-Item $Out).Length / 1KB) } else { 0 }
    "t={0,3}s CPU={1,4:N0}% ({2}) flv={3} KB" -f $t, $tot, ($parts -join ' '), $size
    if ($p.HasExited) { "cmd terminou (RetroArch saiu sozinho)"; break }
}
Get-Process retroarch -EA SilentlyContinue | Stop-Process -Force
Start-Sleep 5
"== ffmpeg ainda vivo? " + [bool](Get-Process ffmpeg -EA SilentlyContinue)
"== RetroArch log (WE Camp / Kaillera / erros):"
Get-Content "logs\retroarch-$Session.log" -EA SilentlyContinue | Select-String -Pattern 'WE Camp|Recording|Kaillera|kaillera|\[ERROR\]|error|Content|Video\]|Audio\]' | Select -First 40 | % { $_.Line }
"== ffmpeg log:"
Get-Content "logs\ffmpeg-$Session.log" -Raw -EA SilentlyContinue | % { ($_ -split "[\r\n]+" | ? { $_ -ne '' }) | Select -Last 12 }
"== saida:"
if (Test-Path $Out) { ffprobe -v error -show_entries format=duration,size:stream=codec_name,width,height,sample_rate -of compact $Out }
