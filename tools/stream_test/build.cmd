@echo off
rem vcvars64.bat: o do C:\TIERES (VS 18 BuildTools) ou, no E:\Projetos, o do VS 2019 BuildTools.
set VCVARS=D:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat
call "%VCVARS%" >nul
cd /d "%~dp0"
cl /nologo /EHsc /D_CRT_SECURE_NO_WARNINGS stream_test.cpp ..\..\common\n02_stream.cpp ..\..\common\nThread.cpp ws2_32.lib user32.lib
