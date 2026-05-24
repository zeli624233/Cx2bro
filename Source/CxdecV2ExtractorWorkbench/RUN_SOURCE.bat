@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title CxdecV2ExtractorWorkbench Launcher

echo ================================================
echo  CxdecV2ExtractorWorkbench Source Launcher
echo ================================================
echo.

if not exist "cxdec_gui_pyside.py" (
    echo [ERROR] cxdec_gui_pyside.py not found.
    echo Please extract the whole zip first, then run this bat from the extracted folder.
    echo.
    pause
    exit /b 1
)

set "PYEXE="
where py >nul 2>nul && set "PYEXE=py -3"
if not defined PYEXE (
    where python >nul 2>nul && set "PYEXE=python"
)
if not defined PYEXE (
    echo [ERROR] Python was not found.
    echo Please install Python 3.10+ first.
    echo.
    pause
    exit /b 1
)

echo [INFO] Checking Python...
%PYEXE% --version
if errorlevel 1 (
    echo [ERROR] Python cannot run.
    echo.
    pause
    exit /b 1
)

echo.
echo [INFO] Checking PySide6...
%PYEXE% -c "import PySide6" >nul 2>nul
if errorlevel 1 (
    echo [INFO] PySide6 is missing. Installing requirements...
    %PYEXE% -m pip install -r requirements.txt
    if errorlevel 1 (
        echo [ERROR] Failed to install requirements.
        echo You can try this manually:
        echo %PYEXE% -m pip install PySide6
        echo.
        pause
        exit /b 1
    )
)

echo.
echo [INFO] Starting GUI...
%PYEXE% cxdec_gui_pyside.py
set "EXITCODE=%errorlevel%"

echo.
echo [INFO] Program exited with code %EXITCODE%.
pause
exit /b %EXITCODE%
