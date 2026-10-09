@echo off
call "D:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /EHsc /D_CRT_SECURE_NO_WARNINGS wecamp_client.cpp ..\..\common\n02_wecamp.cpp ..\..\common\nSettings.cpp winhttp.lib user32.lib
