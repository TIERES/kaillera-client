@echo off
rem WE Camp broadcaster - RetroArch's record_wecamp runs this in place of ffmpeg.exe
rem (the agent sets WECAMP_FFMPEG to it). Until now the agent's "Aguarde" video was on
rem the same YouTube stream, and a stream key takes one sender at a time: stop it, then
rem run the real ffmpeg (WECAMP_FFMPEG_REAL) with RetroArch's arguments as they are.
rem The PID file goes away after the first run, so an ffmpeg restart stops nothing.
rem Stop-Process, not taskkill/tasklist: their /FI filters go through WMI, which says
rem "Acesso negado" outside the SYSTEM task (e.g. over SSH). Only an ffmpeg is stopped.
if defined WECAMP_INTRO_PIDFILE if exist "%WECAMP_INTRO_PIDFILE%" (
  for /f "usebackq delims=" %%p in ("%WECAMP_INTRO_PIDFILE%") do powershell -NoProfile -NonInteractive -Command "Get-Process -Id %%p -ErrorAction SilentlyContinue | Where-Object ProcessName -eq 'ffmpeg' | Stop-Process -Force" >nul 2>&1
  del /q "%WECAMP_INTRO_PIDFILE%" >nul 2>&1
)
rem No pause: ffmpeg takes ~1 s to connect (if YouTube refused it, record_wecamp tries again in 2 s).
if not defined WECAMP_FFMPEG_REAL set "WECAMP_FFMPEG_REAL=ffmpeg.exe"
"%WECAMP_FFMPEG_REAL%" %*
exit /b %errorlevel%
