@echo off
rem Builds TSFix+ and packs the files players need into release\tsfixplus-<version>.zip: the DLL
rem (d3d9.dll) and its settings, INSTALL.txt and the licence.
rem Usage: package.bat 1.0.0
setlocal
cd /d "%~dp0"
if "%~1"=="" (
    echo Usage: package.bat ^<version^>, for example: package.bat 1.0.0
    exit /b 1
)
call "%~dp0build.bat" || exit /b 1

set STAGE=release\tsfixplus-%~1
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%" || exit /b 1
copy /y build\d3d9.dll "%STAGE%\" >nul || exit /b 1
copy /y tsfixplus.ini "%STAGE%\" >nul || exit /b 1
copy /y INSTALL.txt "%STAGE%\" >nul || exit /b 1
copy /y LICENSE "%STAGE%\LICENSE.txt" >nul || exit /b 1

if exist "release\tsfixplus-%~1.zip" del "release\tsfixplus-%~1.zip"
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath 'release\tsfixplus-%~1.zip'" || exit /b 1
echo Packed release\tsfixplus-%~1.zip
