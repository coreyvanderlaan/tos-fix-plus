@echo off
rem Installs TSFix+ in this folder (the Tales of Symphonia game folder). See INSTALL.txt.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tsfixplus-setup.ps1" install
echo.
pause
