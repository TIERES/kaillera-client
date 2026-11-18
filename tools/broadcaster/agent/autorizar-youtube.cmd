@echo off
rem Autoriza o broadcaster a criar lives no canal do YouTube (uma vez so).
rem Rode nesta maquina, numa sessao com navegador, logado na conta Google do canal.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0agent.ps1" -Authorize
pause
