@echo off
rem Removes TSFix+ from this folder and undoes its changes to d3d9.ini and tsfix.ini.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tsfixplus-setup.ps1" uninstall
echo.
pause
