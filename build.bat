@echo off
setlocal

set "ARCH=%~1"
set "CONFIG=%~2"

if "%ARCH%"=="" set "ARCH=x64"
if "%CONFIG%"=="" set "CONFIG=Release"

if /I "%ARCH%"=="x86" set "ARCH=Win32"

where cmake >nul 2>nul
if errorlevel 1 (
    echo CMake was not found in PATH.
    exit /b 1
)

echo Configuring InProcessDumper for %ARCH%...
cmake -S "%~dp0." -B "%~dp0build" -A %ARCH%
if errorlevel 1 exit /b %errorlevel%

echo Building %CONFIG%...
cmake --build "%~dp0build" --config %CONFIG%
if errorlevel 1 exit /b %errorlevel%

echo.
echo Build complete:
echo   %~dp0build\%CONFIG%\InProcessDumper.dll
echo   %~dp0build\%CONFIG%\InProcessDumper.json

endlocal
