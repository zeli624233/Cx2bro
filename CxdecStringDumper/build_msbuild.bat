@echo off
:: 先清空 PATH，再让 vcvars 重新设置
set PATH=C:\Windows\system32;C:\Windows
cd /d "D:\cxdecExtrator.V3.4\KrkrExtractForCxdecV3.3Extra_Plus--\CxdecStringDumper"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
msbuild CxdecStringDumper.vcxproj /p:Configuration=Release /p:Platform=Win32 /t:Build
if errorlevel 1 (
    echo FAILED
    pause
    exit /b 1
)
echo BUILD OK
pause
