@echo off
chcp 65001 >nul
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cd /d "%~dp0"
set DETOURS=%~dp0..\Detours
set ZLIBDIR=%~dp0zlib
set SRCDIR=src
set OUTDIR=Release

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

REM Compile all source files
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\main.obj" "%SRCDIR%\main.cpp"
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\DynamicLauncherLite.obj" "%SRCDIR%\DynamicLauncherLite.cpp"
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\PublisherExtensionBuilderLite.obj" "%SRCDIR%\PublisherExtensionBuilderLite.cpp"
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\PublisherTestUiLite.obj" "%SRCDIR%\PublisherTestUiLite.cpp"
if %ERRORLEVEL% NEQ 0 goto ui_error
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\ResourceRestorerLite.obj" "%SRCDIR%\ResourceRestorerLite.cpp"
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\RestoreUiLite.obj" "%SRCDIR%\RestoreUiLite.cpp"
if %ERRORLEVEL% NEQ 0 goto ui_error
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\StaticHashGeneratorLite.obj" "%SRCDIR%\StaticHashGeneratorLite.cpp"
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\creatwth.obj" "%DETOURS%\creatwth.cpp"
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\detours.obj" "%DETOURS%\detours.cpp"
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\disasm.obj" "%DETOURS%\disasm.cpp"
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\image.obj" "%DETOURS%\image.cpp"
cl /nologo /c /MT /O2 /utf-8 /std:c++17 /EHsc /DUNICODE /D_UNICODE /I"%DETOURS%" /Fo"%OUTDIR%\modules.obj" "%DETOURS%\modules.cpp"

REM Compile zlib source files (required by Krkr text decryption)
cl /nologo /c /MT /O2 /utf-8 /I"%ZLIBDIR%" /Fo"%OUTDIR%\adler32.obj" "%ZLIBDIR%\adler32.c"
cl /nologo /c /MT /O2 /utf-8 /I"%ZLIBDIR%" /Fo"%OUTDIR%\crc32.obj" "%ZLIBDIR%\crc32.c"
cl /nologo /c /MT /O2 /utf-8 /I"%ZLIBDIR%" /Fo"%OUTDIR%\zutil.obj" "%ZLIBDIR%\zutil.c"
cl /nologo /c /MT /O2 /utf-8 /I"%ZLIBDIR%" /Fo"%OUTDIR%\inftrees.obj" "%ZLIBDIR%\inftrees.c"
cl /nologo /c /MT /O2 /utf-8 /I"%ZLIBDIR%" /Fo"%OUTDIR%\inflate.obj" "%ZLIBDIR%\inflate.c"
cl /nologo /c /MT /O2 /utf-8 /I"%ZLIBDIR%" /Fo"%OUTDIR%\inffast.obj" "%ZLIBDIR%\inffast.c"
cl /nologo /c /MT /O2 /utf-8 /I"%ZLIBDIR%" /Fo"%OUTDIR%\uncompr.obj" "%ZLIBDIR%\uncompr.c"

REM Compile resource file (required for RestoreUi dialog template)
rc /nologo /Fo"%OUTDIR%\RestoreUi.res" RestoreUi.rc

REM Link
link /nologo /OUT:"%OUTDIR%\CxdecCoreCLI.exe" /SUBSYSTEM:CONSOLE "%OUTDIR%\main.obj" "%OUTDIR%\DynamicLauncherLite.obj" "%OUTDIR%\PublisherExtensionBuilderLite.obj" "%OUTDIR%\PublisherTestUiLite.obj" "%OUTDIR%\ResourceRestorerLite.obj" "%OUTDIR%\RestoreUiLite.obj" "%OUTDIR%\StaticHashGeneratorLite.obj" "%OUTDIR%\creatwth.obj" "%OUTDIR%\detours.obj" "%OUTDIR%\disasm.obj" "%OUTDIR%\image.obj" "%OUTDIR%\modules.obj" "%OUTDIR%\adler32.obj" "%OUTDIR%\crc32.obj" "%OUTDIR%\zutil.obj" "%OUTDIR%\inftrees.obj" "%OUTDIR%\inflate.obj" "%OUTDIR%\inffast.obj" "%OUTDIR%\uncompr.obj" "%OUTDIR%\RestoreUi.res" comctl32.lib user32.lib kernel32.lib gdi32.lib advapi32.lib shell32.lib comdlg32.lib shlwapi.lib

echo Build completed.

REM Deploy to Cx2bro GUI core directory
mkdir ..\Cx2bro\core 2>nul
taskkill /f /im CxdecCoreCLI.exe >nul 2>nul
copy /Y "Release\CxdecCoreCLI.exe" "..\Cx2bro\core\CxdecCoreCLI.exe" >nul
echo Deployed to ..\Cx2bro\core\CxdecCoreCLI.exe
goto end

:ui_error
echo *** UI compilation error - the VS2022 v143 toolchain is stricter about LPWSTR/LPSTR conversions.
echo *** These files also exist in the D:\cxdecExtrator.V3.4 project and compile there.
echo *** Open the .sln file directly in Visual Studio to build.

:end
pause
