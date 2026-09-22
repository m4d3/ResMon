@echo off
cd /d "%~dp0"
if exist build rmdir /s /q build
if exist package rmdir /s /q package
echo Cleaned build output.
pause
