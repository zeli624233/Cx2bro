@echo off
setlocal enabledelayedexpansion

set ROOT=%~dp0
set SRC=%ROOT%src
set OUT=%ROOT%Release
set TARGET=%OUT%\CxdecDynamicHashCollector.exe
set DEPLOY_TARGET=%ROOT%..\Cx2bro\core\CxdecDynamicHashCollector.exe

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%ROOT%..\Cx2bro\core" mkdir "%ROOT%..\Cx2bro\core"

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 (
    echo [ERROR] VS2022 env init failed
    pause & exit /b 1
)

set SOURCES=
for /R "%SRC%" %%f in (*.cpp) do set "SOURCES=!SOURCES! "%%f""

echo Compiling...
cl /nologo /O2 /MT /W3 /EHsc /DUNICODE /D_UNICODE /utf-8 /Fo"%OUT%\\" /Fe"%TARGET%" %SOURCES% /link shell32.lib comctl32.lib comdlg32.lib user32.lib kernel32.lib gdi32.lib advapi32.lib

if errorlevel 1 (
    echo [ERROR] Build failed
    pause & exit /b 1
)

echo [OK] Build done: %TARGET%

taskkill /f /im CxdecDynamicHashCollector.exe >nul 2>nul
ping -n 2 127.0.0.1 >nul

copy /Y "%TARGET%" "%DEPLOY_TARGET%" >nul
if exist "%DEPLOY_TARGET%" (
    echo [OK] Deployed to %DEPLOY_TARGET%
) else (
    echo [WARN] Deploy failed
)
