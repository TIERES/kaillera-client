@echo off
rem WE Camp broadcaster - watches one live session and streams it.
rem   start-broadcast.cmd SESSION_ID "GAME FILE" OUTPUT [ROOM NAME]
rem OUTPUT = rtmp://a.rtmp.youtube.com/live2/<stream key>  or a file (.flv/.mkv) for tests.
rem RetroArch quits by itself when the session ends (or if it does not start in 120 s).
rem Optional overrides: WECAMP_STREAM_SIZE (640x480), WECAMP_STREAM_FPS (30),
rem WECAMP_STREAM_VBITRATE (1500k), WECAMP_STREAM_ABITRATE (128k),
rem WECAMP_STREAM_PRESET (veryfast), WECAMP_FFMPEG (ffmpeg.exe in PATH),
rem WECAMP_BROADCAST_FROM_FRAME (fast-forward, not streamed, up to this frame),
rem WECAMP_CONTROL_FILE ("stop_frame=N" ends the stream at frame N),
rem WECAMP_LOG_NAME (log file names; default = the session id).
setlocal
if "%~3"=="" (
  echo uso: %~nx0 SESSION_ID "ARQUIVO DO JOGO" SAIDA [SALA]
  exit /b 2
)
cd /d "%~dp0"
if not exist logs mkdir logs
set WECAMP_BROADCASTER=1
set "WECAMP_WATCH_SESSION_ID=%~1"
set "WECAMP_STREAM_URL=%~3"
set "WECAMP_WATCH_ROOM=%~4"
if not defined WECAMP_LOG_NAME set "WECAMP_LOG_NAME=%~1"
if not defined WECAMP_FFMPEG (
  where ffmpeg.exe >nul 2>&1 || set "WECAMP_FFMPEG=C:\Program Files\WinGet\Packages\Gyan.FFmpeg.Shared_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0.2-full_build-shared\bin\ffmpeg.exe"
)
set "WECAMP_FFMPEG_LOG=logs\ffmpeg-%WECAMP_LOG_NAME%.log"
echo [%date% %time%] inicio sessao %~1 (%WECAMP_LOG_NAME%) >> logs\broadcaster.log
retroarch.exe --config retroarch.cfg -v --log-file "logs\retroarch-%WECAMP_LOG_NAME%.log" -L cores\pcsx_rearmed_libretro.dll "%~2"
echo [%date% %time%] fim sessao %~1 (%WECAMP_LOG_NAME%, saida %errorlevel%) >> logs\broadcaster.log
