@echo off
rem Builds TOSFIXPLUS and packs the files players need into release\tosfixplus-<version>.zip.
rem Usage: package.bat 1.0.0
setlocal
cd /d "%~dp0"
if "%~1"=="" (
    echo Usage: package.bat ^<version^>, for example: package.bat 1.0.0
    exit /b 1
)
call "%~dp0build.bat" || exit /b 1

set STAGE=release\tosfixplus-%~1
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%" || exit /b 1
copy /y build\tosfixplus.dll "%STAGE%\" >nul || exit /b 1
copy /y tosfixplus.ini "%STAGE%\" >nul || exit /b 1
copy /y INSTALL.txt "%STAGE%\" >nul || exit /b 1
copy /y LICENSE "%STAGE%\LICENSE.txt" >nul || exit /b 1

if exist "release\tosfixplus-%~1.zip" del "release\tosfixplus-%~1.zip"
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath 'release\tosfixplus-%~1.zip'" || exit /b 1
echo Packed release\tosfixplus-%~1.zip
