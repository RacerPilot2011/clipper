@echo off
REM Build script for Windows using vcpkg

echo ========================================
echo Screen Clip Recorder - Windows Builder
echo ========================================
echo.

rmdir /s /q build
REM Check if vcpkg is installed
if not exist "C:\vcpkg" (
    echo ERROR: vcpkg not found at C:\vcpkg
    echo.
    echo Please install vcpkg first:
    echo   git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg
    echo   cd C:\vcpkg
    echo   .\bootstrap-vcpkg.bat
    echo.
    pause
    exit /b 1
)

REM Install dependencies
echo Installing dependencies via vcpkg...
echo This may take a while on first run...
echo.

C:\vcpkg\vcpkg.exe install ^
    qtbase:x64-windows ^
    qtmultimedia:x64-windows ^
    qtimageformats:x64-windows ^
    opencv4:x64-windows ^
    ffmpeg:x64-windows ^
    curl:x64-windows

if %ERRORLEVEL% neq 0 (
    echo.
    echo ERROR: Failed to install dependencies
    pause
    exit /b 1
)

REM Create build directory
if not exist "build" mkdir build
cd build

REM Configure with CMake
echo.
echo Configuring project...
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake

if %ERRORLEVEL% neq 0 (
    echo.
    echo ERROR: CMake configuration failed
    cd ..
    pause
    exit /b 1
)

REM Build
echo.
echo Building project...
cmake --build . --config Release

if %ERRORLEVEL% neq 0 (
    echo.
    echo ERROR: Build failed
    cd ..
    pause
    exit /b 1
)

cd ..

echo.
echo ========================================
echo Build completed successfully!
echo ========================================
echo.
echo Deploying Qt dependencies...
echo.

REM Deploy Qt platform plugin and DLLs
set BUILD_DIR=build\Release
set VCPKG_DIR=C:\vcpkg\installed\x64-windows

echo Copying essential Qt DLLs...
copy /Y "%VCPKG_DIR%\bin\Qt6Core.dll" "%BUILD_DIR%\" >nul 2>&1
copy /Y "%VCPKG_DIR%\bin\Qt6Gui.dll" "%BUILD_DIR%\" >nul 2>&1
copy /Y "%VCPKG_DIR%\bin\Qt6Widgets.dll" "%BUILD_DIR%\" >nul 2>&1
copy /Y "%VCPKG_DIR%\bin\Qt6Network.dll" "%BUILD_DIR%\" >nul 2>&1

echo Copying OpenCV DLLs...
copy /Y "%VCPKG_DIR%\bin\opencv_world4*.dll" "%BUILD_DIR%\" >nul 2>&1

echo Creating platforms directory and copying Qt platform plugin...
if not exist "%BUILD_DIR%\platforms" mkdir "%BUILD_DIR%\platforms"
copy /Y "%VCPKG_DIR%\Qt6\plugins\platforms\qwindows.dll" "%BUILD_DIR%\platforms\" >nul 2>&1

echo Creating imageformats directory and copying image plugins...
if not exist "%BUILD_DIR%\imageformats" mkdir "%BUILD_DIR%\imageformats"
copy /Y "%VCPKG_DIR%\Qt6\plugins\imageformats\qjpeg.dll" "%BUILD_DIR%\imageformats\" >nul 2>&1
copy /Y "%VCPKG_DIR%\Qt6\plugins\imageformats\qgif.dll" "%BUILD_DIR%\imageformats\" >nul 2>&1
copy /Y "%VCPKG_DIR%\Qt6\plugins\imageformats\qico.dll" "%BUILD_DIR%\imageformats\" >nul 2>&1
copy /Y "%VCPKG_DIR%\Qt6\plugins\imageformats\qpng.dll" "%BUILD_DIR%\imageformats\" >nul 2>&1
copy /Y "%VCPKG_DIR%\Qt6\plugins\imageformats\*.dll" "%BUILD_DIR%\imageformats\" >nul 2>&1
echo Image format plugins copied

echo Copying all vcpkg DLLs (this ensures all dependencies are present)...
copy /Y "%VCPKG_DIR%\bin\*.dll" "%BUILD_DIR%\" >nul 2>&1
echo.
echo Copying FFmpeg DLLs...
copy /Y "%VCPKG_DIR%\bin\avcodec-*.dll" "%BUILD_DIR%\"
copy /Y "%VCPKG_DIR%\bin\avformat-*.dll" "%BUILD_DIR%\"
copy /Y "%VCPKG_DIR%\bin\avutil-*.dll" "%BUILD_DIR%\"
copy /Y "%VCPKG_DIR%\bin\swscale-*.dll" "%BUILD_DIR%\"
copy /Y "%VCPKG_DIR%\bin\swresample-*.dll" "%BUILD_DIR%\"

echo.
echo Copying other dependencies...
copy /Y "%VCPKG_DIR%\bin\zlib1.dll" "%BUILD_DIR%\" 2>nul
copy /Y "%VCPKG_DIR%\bin\libcurl.dll" "%BUILD_DIR%\" 2>nul

echo.
echo Setting up FFmpeg...
if exist "%VCPKG_DIR%\tools\ffmpeg\ffmpeg.exe" (
    copy /Y "%VCPKG_DIR%\tools\ffmpeg\ffmpeg.exe" "%BUILD_DIR%\" >nul 2>&1
    echo FFmpeg executable copied
) else (
    echo WARNING: FFmpeg executable not found in vcpkg tools
    echo Run setup_ffmpeg.bat to install it
)

echo.
echo Verifying deployment...
if exist "%BUILD_DIR%\imageformats\qjpeg.dll" (
    echo [OK] JPEG plugin found
) else (
    echo [WARNING] JPEG plugin missing!
)

if exist "%BUILD_DIR%\platforms\qwindows.dll" (
    echo [OK] Windows platform plugin found
) else (
    echo [WARNING] Windows platform plugin missing!
)

echo.
echo ========================================
echo Deployment complete!
echo ========================================
echo.
echo Executable location: build\Release\ScreenClipRecorder.exe
echo.
echo To run the application:
echo   1. Use the quick launcher: run_app.bat
echo   2. Or navigate to build\Release and run ScreenClipRecorder.exe
echo.