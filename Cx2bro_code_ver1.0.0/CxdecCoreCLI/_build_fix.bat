@echo off
chcp 65001 >nul
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cd /d "%~dp0"

set DETOURS=D:\cxdecExtrator.V3.4\KrkrExtractForCxdecV3.3Extra_Plus--\Detours
set SRCDIR=src
set OUTDIR=Release

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

REM Compile source files
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\main.obj" "%SRCDIR%\main.cpp"
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\DynamicLauncherLite.obj" "%SRCDIR%\DynamicLauncherLite.cpp"
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\PublisherExtensionBuilderLite.obj" "%SRCDIR%\PublisherExtensionBuilderLite.cpp"
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\PublisherTestUiLite.obj" "%SRCDIR%\PublisherTestUiLite.cpp"
if %ERRORLEVEL% NEQ 0 goto ui_error
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\ResourceRestorerLite.obj" "%SRCDIR%\ResourceRestorerLite.cpp"
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\RestoreUiLite.obj" "%SRCDIR%\RestoreUiLite.cpp"
if %ERRORLEVEL% NEQ 0 goto ui_error
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\StaticHashGeneratorLite.obj" "%SRCDIR%\StaticHashGeneratorLite.cpp"

REM Detours
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\creatwth.obj" "%DETOURS%\creatwth.cpp"
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\detours.obj" "%DETOURS%\detours.cpp"
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\disasm.obj" "%DETOURS%\disasm.cpp"
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\image.obj" "%DETOURS%\image.cpp"
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\modules.obj" "%DETOURS%\modules.cpp"

REM Compile resource
rc /nologo /fo"%OUTDIR%\RestoreUi.res" RestoreUi.rc
if %ERRORLEVEL% NEQ 0 goto rc_error

REM Link
link /nologo /OUT:"%OUTDIR%\CxdecCoreCLI.exe" /SUBSYSTEM:CONSOLE "%OUTDIR%\main.obj" "%OUTDIR%\DynamicLauncherLite.obj" "%OUTDIR%\PublisherExtensionBuilderLite.obj" "%OUTDIR%\PublisherTestUiLite.obj" "%OUTDIR%\ResourceRestorerLite.obj" "%OUTDIR%\RestoreUiLite.obj" "%OUTDIR%\StaticHashGeneratorLite.obj" "%OUTDIR%\creatwth.obj" "%OUTDIR%\detours.obj" "%OUTDIR%\disasm.obj" "%OUTDIR%\image.obj" "%OUTDIR%\modules.obj" "%OUTDIR%\RestoreUi.res" comctl32.lib shell32.lib user32.lib kernel32.lib gdi32.lib advapi32.lib
if %ERRORLEVEL% NEQ 0 goto link_error

echo Build completed: Release\CxdecCoreCLI.exe

REM Deploy
mkdir ..\Cx2bro\core 2>nul
taskkill /f /im CxdecCoreCLI.exe >nul 2>nul
copy /Y "Release\CxdecCoreCLI.exe" "..\Cx2bro\core\CxdecCoreCLI.exe" >nul
echo Deployed to ..\Cx2bro\core\CxdecCoreCLI.exe
goto end

:rc_error
echo Resource compilation failed - check RestoreUi.rc
goto end

:ui_error
echo UI compilation error - see VS2022 v143 strictness
goto end

:link_error
echo Link failed

:end
pause
