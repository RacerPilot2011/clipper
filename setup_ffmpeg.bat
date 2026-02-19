@echo off
REM Find and deploy FFmpeg for ScreenClipRecorder

echo ========================================
echo FFmpeg Setup for ScreenClipRecorder
echo ========================================
echo.

set BUILD_DIR=build\Release
set VCPKG_DIR=C:\vcpkg\installed\x64-windows

REM Check for ffmpeg in vcpkg tools
if exist "%VCPKG_DIR%\tools\ffmpeg\ffmpeg.exe" (
    echo Found FFmpeg in vcpkg tools directory
    copy /Y "%VCPKG_DIR%\tools\ffmpeg\ffmpeg.exe" "%BUILD_DIR%\"
    echo FFmpeg copied to %BUILD_DIR%
    echo.
    echo ========================================
    echo FFmpeg setup complete!
    echo ========================================
    goto :end
)

REM Check for ffmpeg in vcpkg bin
if exist "%VCPKG_DIR%\bin\ffmpeg.exe" (
    echo Found FFmpeg in vcpkg bin directory
    copy /Y "%VCPKG_DIR%\bin\ffmpeg.exe" "%BUILD_DIR%\"
    echo FFmpeg copied to %BUILD_DIR%
    echo.
    echo ========================================
    echo FFmpeg setup complete!
    echo ========================================
    goto :end
)

REM Try to install ffmpeg with vcpkg
echo FFmpeg not found in vcpkg, attempting to install...
echo.
C:\vcpkg\vcpkg.exe install ffmpeg[ffmpeg]:x64-windows

if exist "%VCPKG_DIR%\tools\ffmpeg\ffmpeg.exe" (
    copy /Y "%VCPKG_DIR%\tools\ffmpeg\ffmpeg.exe" "%BUILD_DIR%\"
    echo.
    echo ========================================
    echo FFmpeg installed and copied!
    echo ========================================
    goto :end
)

echo.
echo ========================================
echo WARNING: FFmpeg not found!
echo ========================================
echo.
echo FFmpeg is required for video encoding.
echo.
echo Please install FFmpeg manually:
echo 1. Download from: https://www.gyan.dev/ffmpeg/builds/
echo 2. Extract ffmpeg.exe
echo 3. Copy it to: %BUILD_DIR%
echo.
echo Or install via vcpkg:
echo   vcpkg install ffmpeg[ffmpeg]:x64-windows
echo.

:end
pause