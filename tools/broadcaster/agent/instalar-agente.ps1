<#
  Instala (ou reinstala) o agente do broadcaster como tarefa agendada. Rode como Administrador:
  - roda como SYSTEM, sem ninguem logado: na inicializacao do Windows e, se nao estiver
    rodando, de novo a cada 5 min (o agente sai sozinho quando o agent.ps1 ou o config.json
    mudam, para carregar a versao nova);
  - para o agente que estiver rodando antes de reinstalar;
  - energia: nunca suspender/hibernar na tomada e "fechar a tampa = nada".
#>
$ErrorActionPreference = 'Stop'
$task = 'WE Camp Broadcaster'
$agent = Join-Path $PSScriptRoot 'agent.ps1'
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Rode como Administrador.'
}
if (-not (Test-Path (Join-Path $PSScriptRoot 'config.json'))) { throw 'Crie o config.json (a partir do config.example.json) antes.' }

# O agente que estiver rodando (versao antiga) para antes.
Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe'" |
    Where-Object { $_.CommandLine -like '*agent.ps1*' -and $_.ProcessId -ne $PID } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

$action = New-ScheduledTaskAction -Execute 'powershell.exe' -WorkingDirectory $PSScriptRoot `
    -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$agent`""
$atStartup = New-ScheduledTaskTrigger -AtStartup
$every5min = New-ScheduledTaskTrigger -Once -At (Get-Date).Date -RepetitionInterval (New-TimeSpan -Minutes 5) `
    -RepetitionDuration (New-TimeSpan -Days 3650)
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable `
    -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName $task -Action $action -Trigger @($atStartup, $every5min) -Principal $principal `
    -Settings $settings -Force | Out-Null

# Energia (na tomada): nunca suspender nem hibernar, tampa fechada = nada.
powercfg /change standby-timeout-ac 0
powercfg /change hibernate-timeout-ac 0
powercfg /setacvalueindex SCHEME_CURRENT SUB_BUTTONS LIDACTION 0
powercfg /setactive SCHEME_CURRENT

Start-ScheduledTask -TaskName $task
Write-Host "Tarefa `"$task`" instalada e iniciada. Log: C:\broadcaster\logs\agent.log"
