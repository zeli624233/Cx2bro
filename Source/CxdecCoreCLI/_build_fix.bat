@echo off
chcp 65001 >nul
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cd /d "%~dp0"
set DETOURS=D:\cxdecExtrator.V3.4\KrkrExtractForCxdecV3.3Extra_Plus--\Detours
set SRCDIR=src
set OUTDIR=Release

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

REM Compile all source files
cl /nologo /c /MD /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\main.obj" "%SRCDIR%\main.cpp"
cl /nologo /c /MD /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\DynamicLauncherLite.obj" "%SRCDIR%\DynamicLauncherLite.cpp"
cl /nologo /c /MD /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\PublisherExtensionBuilderLite.obj" "%SRCDIR%\PublisherExtensionBuilderLite.cpp"
cl /nologo /c /MD /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\PublisherTestUiLite.obj" "%SRCDIR%\PublisherTestUiLite.cpp"
if %ERRORLEVEL% NEQ 0 goto ui_error
cl /nologo /c /MD /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\ResourceRestorerLite.obj" "%SRCDIR%\ResourceRestorerLite.cpp"
cl /nologo /c /MD /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\RestoreUiLite.obj" "%SRCDIR%\RestoreUiLite.cpp"
if %ERRORLEVEL% NEQ 0 goto ui_error
cl /nologo /c /MD /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\StaticHashGeneratorLite.obj" "%SRCDIR%\StaticHashGeneratorLite.cpp"
cl /nologo /c /MD /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\creatwth.obj" "%DETOURS%\creatwth.cpp"
cl /nologo /c /MD /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\detours.obj" "%DETOURS%\detours.cpp"
cl /nologo /c /MD /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\disasm.obj" "%DETOURS%\disasm.cpp"
cl /nologo /c /MD /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\image.obj" "%DETOURS%\image.cpp"
cl /nologo /c /MD /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\modules.obj" "%DETOURS%\modules.cpp"

REM Link
link /nologo /OUT:"%OUTDIR%\CxdecCoreCLI.exe" /SUBSYSTEM:CONSOLE "%OUTDIR%\main.obj" "%OUTDIR%\DynamicLauncherLite.obj" "%OUTDIR%\PublisherExtensionBuilderLite.obj" "%OUTDIR%\PublisherTestUiLite.obj" "%OUTDIR%\ResourceRestorerLite.obj" "%OUTDIR%\RestoreUiLite.obj" "%OUTDIR%\StaticHashGeneratorLite.obj" "%OUTDIR%\creatwth.obj" "%OUTDIR%\detours.obj" "%OUTDIR%\disasm.obj" "%OUTDIR%\image.obj" "%OUTDIR%\modules.obj" comctl32.lib

echo Build completed.
goto end

:ui_error
echo *** UI compilation error - the VS2022 v143 toolchain is stricter about LPWSTR/LPSTR conversions.
echo *** These files also exist in the D:\cxdecExtrator.V3.4 project and compile there.
echo *** Open the .sln file directly in Visual Studio to build.

:end
pause
