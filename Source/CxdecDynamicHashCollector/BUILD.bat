@echo off
setlocal enabledelayedexpansion

:: ⚠ 注意：新版源码已在 cxdecv2，此处改为指向统一源码仓库
:: 如需本地调试可改回 %~dp0src
set ROOT=%~dp0..\..\..\cxdecv2\CxdecDynamicHashCollector\
set SRC=%ROOT%src
set OUT=%ROOT%Release
set TARGET=%OUT%\CxdecDynamicHashCollector.exe

if not exist "%OUT%" mkdir "%OUT%"

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86
if errorlevel 1 (
    echo [ERROR] Failed to initialize VS2022 environment
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

echo [OK] Build succeeded: %TARGET%

:: Deploy to workbench core directory
set DEPLOY_TARGET=%~dp0..\CxdecV2ExtractorWorkbench\core\CxdecDynamicHashCollector.exe

:: 部署前先杀旧进程，防止文件被占用导致部署失败
taskkill /f /im CxdecDynamicHashCollector.exe >nul 2>nul
ping -n 2 127.0.0.1 >nul

copy /Y "%TARGET%" "%DEPLOY_TARGET%" >nul
if exist "%DEPLOY_TARGET%" (
    echo [OK] Deployed to core: %DEPLOY_TARGET%
) else (
    echo [WARN] Core directory not found, skip deploy
)
