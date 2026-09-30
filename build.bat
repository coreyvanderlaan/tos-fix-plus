@echo off
rem Builds build\d3d9.dll (32-bit, like the game) with the Microsoft C++ compiler.
rem Needs Visual Studio 2019 or later, or the Build Tools, with "Desktop development with C++".
setlocal
cd /d "%~dp0"

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
    echo Visual Studio or its Build Tools were not found.
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
if not defined VS (
    echo No Visual Studio installation with the C++ tools was found.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul || exit /b 1

if not exist build mkdir build
cl /nologo /LD /O2 /EHsc /MT /W3 /std:c++17 /D_CRT_SECURE_NO_WARNINGS ^
   src\main.cpp src\recorder.cpp src\interpolate.cpp src\standalone.cpp src\textures.cpp src\lzma\*.c ^
   /Fo:build\ /Fe:build\d3d9.dll ^
   /link /DEF:src\tsfixplus.def user32.lib || exit /b 1
copy /y tsfixplus.ini build\ >nul
echo Built build\d3d9.dll
