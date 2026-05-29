@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title CxdecV2ExtractorWorkbench Debug Launcher
set "LOG=%~dp0startup_error_log.txt"

echo ================================================
echo  CxdecV2ExtractorWorkbench Debug Launcher
echo ================================================
echo.
echo This window will not close automatically.
echo If startup fails, send startup_error_log.txt.
echo.

> "%LOG%" echo ===== CxdecV2ExtractorWorkbench startup log =====
>> "%LOG%" echo Date: %date% %time%
>> "%LOG%" echo WorkDir: %cd%
>> "%LOG%" echo.

if not exist "cxdec_gui_pyside.py" (
    echo [ERROR] cxdec_gui_pyside.py not found.
    >> "%LOG%" echo [ERROR] cxdec_gui_pyside.py not found.
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
    >> "%LOG%" echo [ERROR] Python was not found.
    pause
    exit /b 1
)

echo [INFO] Python command: %PYEXE%
>> "%LOG%" echo [INFO] Python command: %PYEXE%
%PYEXE% --version >> "%LOG%" 2>&1

echo [INFO] Checking PySide6...
>> "%LOG%" echo [INFO] Checking PySide6...
%PYEXE% -c "import PySide6; print(PySide6.__version__)" >> "%LOG%" 2>&1
if errorlevel 1 (
    echo [INFO] PySide6 missing. Installing requirements...
    >> "%LOG%" echo [INFO] PySide6 missing. Installing requirements...
    %PYEXE% -m pip install -r requirements.txt >> "%LOG%" 2>&1
    if errorlevel 1 (
        echo [ERROR] Failed to install requirements. See startup_error_log.txt.
        >> "%LOG%" echo [ERROR] Failed to install requirements.
        pause
        exit /b 1
    )
)

echo [INFO] Running syntax check...
>> "%LOG%" echo [INFO] Running syntax check...
%PYEXE% -m py_compile cxdec_gui_pyside.py version.py >> "%LOG%" 2>&1
if errorlevel 1 (
    echo [ERROR] Syntax check failed. See startup_error_log.txt.
    pause
    exit /b 1
)

echo [INFO] Starting GUI...
>> "%LOG%" echo [INFO] Starting GUI...
%PYEXE% cxdec_gui_pyside.py >> "%LOG%" 2>&1
set "EXITCODE=%errorlevel%"
>> "%LOG%" echo.
>> "%LOG%" echo [INFO] Program exited with code %EXITCODE%.

echo.
echo [INFO] Program exited with code %EXITCODE%.
echo [INFO] Log file: startup_error_log.txt
pause
exit /b %EXITCODE%
