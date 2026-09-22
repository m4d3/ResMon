@echo off
setlocal
cd /d "%~dp0"
set "EXE=%CD%\build\Release\ResMon.exe"
if not exist "%EXE%" set "EXE=%CD%\build\ResMon.exe"
if not exist "%EXE%" (
    echo ResMon.exe has not been built yet.
    echo Run BUILD_AND_RUN.cmd first.
    pause
    exit /b 1
)
start "" "%EXE%"
