@echo off
REM Build script for Windows using vcpkg

echo ========================================
echo Screen Clip Recorder - Windows Builder
echo ========================================
echo.

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
echo Executable location: build\Release\ScreenClipRecorder.exe
echo.
echo To run the application:
echo   cd build\Release
echo   ScreenClipRecorder.exe
echo.
pause