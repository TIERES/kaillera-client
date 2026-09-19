@echo off
setlocal enabledelayedexpansion
REM Build script for n02 Kaillera DLL
REM This builds the 32-bit Release version

set PROJECT="%~dp0n02p.vcxproj"

REM Try to find MSBuild in common locations
set MSBUILD=
set TOOLSET=

REM Use vswhere to locate MSBuild regardless of install drive/edition
set VSWHERE="C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if exist %VSWHERE% (
    for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -prerelease -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
        set MSBUILD="%%i"
    )
    for /f "usebackq tokens=1 delims=." %%v in (`%VSWHERE% -latest -prerelease -products * -requires Microsoft.Component.MSBuild -property installationVersion`) do (
        set VSMAJOR=%%v
    )
    if defined MSBUILD (
        if "!VSMAJOR!"=="18" set TOOLSET=v145
        if "!VSMAJOR!"=="17" set TOOLSET=v143
        if "!VSMAJOR!"=="16" set TOOLSET=v142
        goto :found
    )
)

REM VS2026 (v18) Community
if exist "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" (
    set MSBUILD="C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
    set TOOLSET=v145
    goto :found
)

REM VS2026 (v18) Professional
if exist "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe" (
    set MSBUILD="C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe"
    set TOOLSET=v145
    goto :found
)

REM VS2026 (v18) Enterprise
if exist "C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe" (
    set MSBUILD="C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
    set TOOLSET=v145
    goto :found
)

REM VS2022 Community
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" (
    set MSBUILD="C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
    set TOOLSET=v143
    goto :found
)

REM VS2022 Professional
if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" (
    set MSBUILD="C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe"
    set TOOLSET=v143
    goto :found
)

REM VS2022 Enterprise
if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe" (
    set MSBUILD="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
    set TOOLSET=v143
    goto :found
)

REM VS2022 Build Tools
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" (
    set MSBUILD="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
    set TOOLSET=v143
    goto :found
)

REM VS2019 Build Tools
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\MSBuild.exe" (
    set MSBUILD="C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
    set TOOLSET=v142
    goto :found
)

REM VS2019 Community
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe" (
    set MSBUILD="C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe"
    set TOOLSET=v142
    goto :found
)

echo ERROR: Could not find MSBuild.exe
echo Please install Visual Studio 2019 or 2022 with C++ build tools
pause
exit /b 1

:found
echo Found MSBuild: %MSBUILD%
echo Using PlatformToolset: %TOOLSET%
echo.
echo Building n02 (32-bit Release)...
%MSBUILD% %PROJECT% /p:Configuration=Release /p:Platform=Win32 /p:WindowsTargetPlatformVersion=10.0 /p:PlatformToolset=%TOOLSET%

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Build successful!
    echo Output: %~dp0Release\n02p.dll
    echo.
    echo To use with a 32-bit emulator, copy n02p.dll to its folder as kailleraclient.dll
) else (
    echo.
    echo Build failed with error code %ERRORLEVEL%
)

pause
