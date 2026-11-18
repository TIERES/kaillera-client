param([string]$Session = '36368-1794977438', [string]$Game = 'games\WE PW26 INTERNATIONAL - WE2002.chd',
      [string]$Out = 'C:\broadcaster\out\teste-completo.flv', [int]$MaxMinutes = 40)
# Full-session test: start-broadcast.cmd must end by itself when the session ends.
Set-Location C:\broadcaster
New-Item -ItemType Directory -Force out | Out-Null
Remove-Item $Out, "logs\retroarch-$Session.log", "logs\ffmpeg-$Session.log" -EA SilentlyContinue
$t0 = Get-Date
$p = Start-Process cmd -ArgumentList "/c start-broadcast.cmd $Session `"$Game`" $Out" -PassThru -WindowStyle Hidden
$ncpu = [Environment]::ProcessorCount; $prev = @{}; $samples = @()
Add-Type -AssemblyName Microsoft.VisualBasic; $ci = New-Object Microsoft.VisualBasic.Devices.ComputerInfo
while (-not $p.HasExited -and ((Get-Date) - $t0).TotalMinutes -lt $MaxMinutes) {
    Start-Sleep 60
    $tot = 0; $parts = @()
    foreach ($pr in Get-Process retroarch, ffmpeg -EA SilentlyContinue) {
        $d = $pr.CPU - [double]$prev[$pr.Id]; $prev[$pr.Id] = $pr.CPU
        $u = 100 * $d / 60 / $ncpu; $tot += $u; $parts += ("{0}={1:N0}%" -f $pr.Name, $u)
    }
    $samples += $tot
    $freeGB = $ci.AvailablePhysicalMemory / 1GB
    "{0,5:N1} min CPU={1,4:N0}% ({2}) RAM livre={3:N1} GB" -f ((Get-Date) - $t0).TotalMinutes, $tot, ($parts -join ' '), $freeGB
}
if (-not $p.HasExited) { "TIMEOUT - RetroArch nao saiu sozinho"; Get-Process retroarch -EA SilentlyContinue | Stop-Process -Force; Start-Sleep 5 }
"== duracao total: {0:N1} min; CPU media {1:N0}%, maxima {2:N0}%" -f ((Get-Date) - $t0).TotalMinutes, ($samples | Measure -Average).Average, ($samples | Measure -Maximum).Maximum
"== processos restantes: " + ((Get-Process retroarch, ffmpeg -EA SilentlyContinue | % Name) -join ', ')
"== RetroArch log ([WE Camp]):"
Get-Content "logs\retroarch-$Session.log" -EA SilentlyContinue | Select-String 'WE Camp|ERROR' | % { $_.Line }
"== ffmpeg log (fim):"
Get-Content "logs\ffmpeg-$Session.log" -Raw -EA SilentlyContinue | % { ($_ -split "[\r\n]+" | ? { $_ -ne '' }) | Select -Last 4 }
"== broadcaster.log:"; Get-Content logs\broadcaster.log | Select -Last 2
"== saida:"
if (Test-Path $Out) { ffprobe -v error -show_entries format=duration,size,bit_rate -of compact $Out }
