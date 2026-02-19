@echo off
REM Download and setup VCodecLibav library

echo ========================================
echo VCodecLibav Setup
echo ========================================
echo.

set BUILD_DIR=%cd%
set VCODEC_DIR=%BUILD_DIR%\VCodecLibav

echo Checking for VCodecLibav...
if exist "%VCODEC_DIR%" (
    echo VCodecLibav already exists at: %VCODEC_DIR%
    echo.
    choice /M "Do you want to re-download"
    if errorlevel 2 goto :skip_download
    rd /s /q "%VCODEC_DIR%"
)

echo.
echo Cloning VCodecLibav from GitHub...
git clone https://github.com/ConstantRobotics-Ltd/VCodec.git "%VCODEC_DIR%"

if not exist "%VCODEC_DIR%" (
    echo.
    echo ERROR: Failed to clone VCodecLibav
    echo.
    echo Please manually download from:
    echo https://github.com/ConstantRobotics-Ltd/VCodec.git
    echo.
    echo Extract to: %VCODEC_DIR%
    pause
    exit /b 1
)

:skip_download
echo.
echo VCodecLibav ready at: %VCODEC_DIR%
echo.
echo Next steps:
echo 1. Run build_windows.bat to compile with VCodecLibav support
echo 2. The encoder will use H.264 via libavcodec
echo.
pause