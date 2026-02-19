@echo off
REM Quick launcher for ScreenClipRecorder with console output

echo ========================================
echo Screen Clip Recorder
echo ========================================
echo.

cd /d "%~dp0"

if not exist "build\Release\ScreenClipRecorder.exe" (
    echo ERROR: Application not built yet!
    echo.
    echo Please run build_windows.bat first
    pause
    exit /b 1
)

REM Add vcpkg bin directory to PATH so DLLs can be found
set PATH=C:\vcpkg\installed\x64-windows\bin;%PATH%

REM Set Qt plugin path
set QT_PLUGIN_PATH=C:\vcpkg\installed\x64-windows\plugins

echo Starting ScreenClipRecorder...
echo Console output will appear below:
echo ========================================
echo.

cd build\Release
ScreenClipRecorder.exe

echo.
echo ========================================
echo Application closed.