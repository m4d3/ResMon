@echo off
setlocal EnableExtensions
cd /d "%~dp0"

echo ================================================
echo   ResMon - Release Build + Run
echo ================================================
echo.

where cmake >nul 2>nul
if errorlevel 1 (
    echo ERROR: CMake was not found in PATH.
    echo Install CMake and Visual Studio Build Tools with the C++ workload.
    pause
    exit /b 1
)

echo Preparing embedded enhanced-temperature resources...
powershell -NoProfile -ExecutionPolicy Bypass -File "%CD%\FETCH_SENSOR_DEPS.ps1"
if errorlevel 1 (
    echo.
    echo ERROR: Could not download or verify the optional PawnIO sensor resources.
    echo Check your Internet connection, then run BUILD_AND_RUN.cmd again.
    pause
    exit /b 1
)
echo.

if exist build rmdir /s /q build

set "VS_GENERATOR="
cmake --help | findstr /C:"Visual Studio 18 2026" >nul 2>nul
if not errorlevel 1 set "VS_GENERATOR=Visual Studio 18 2026"
if not defined VS_GENERATOR (
    cmake --help | findstr /C:"Visual Studio 17 2022" >nul 2>nul
    if not errorlevel 1 set "VS_GENERATOR=Visual Studio 17 2022"
)

if defined VS_GENERATOR (
    echo Using CMake generator: %VS_GENERATOR%
    cmake -S . -B build -G "%VS_GENERATOR%" -A x64
) else (
    echo No Visual Studio generator was detected by CMake; trying the default generator.
    cmake -S . -B build -A x64
    if errorlevel 1 (
        if exist build rmdir /s /q build
        cmake -S . -B build
    )
)
if errorlevel 1 goto :build_failed

cmake --build build --config Release --parallel
if errorlevel 1 goto :build_failed

set "EXE=%CD%\build\Release\ResMon.exe"
if not exist "%EXE%" set "EXE=%CD%\build\ResMon.exe"

if not exist "%EXE%" (
    echo ERROR: Build completed but ResMon.exe was not found.
    pause
    exit /b 1
)

echo.
echo Build complete:
echo   %EXE%
echo.
start "" "%EXE%"
exit /b 0

:build_failed
echo.
echo BUILD FAILED.
echo.
echo CMake successfully started the compiler. Read the first compiler error above.
echo If the error says a Windows header or library is missing, verify the Windows 11 SDK.
echo Otherwise the failure is in the source and does not require reinstalling Visual Studio.
pause
exit /b 1
