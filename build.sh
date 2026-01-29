#!/bin/bash

echo "========================================"
echo "Screen Clip Recorder - Build Script"
echo "========================================"
echo ""

# Detect OS
OS="$(uname -s)"
case "${OS}" in
    Linux*)     MACHINE=Linux;;
    Darwin*)    MACHINE=Mac;;
    *)          MACHINE="UNKNOWN:${OS}"
esac

echo "Detected OS: $MACHINE"
echo ""

# Install dependencies based on OS
if [ "$MACHINE" == "Linux" ]; then
    echo "Installing Linux dependencies..."
    echo ""
    
    # Check if running as root
    if [ "$EUID" -ne 0 ]; then 
        SUDO="sudo"
    else
        SUDO=""
    fi
    
    # Detect package manager
    if command -v apt-get &> /dev/null; then
        echo "Using apt package manager..."
        $SUDO apt-get update
        $SUDO apt-get install -y \
            build-essential cmake \
            qt6-base-dev qt6-multimedia-dev \
            libopencv-dev \
            libavcodec-dev libavformat-dev libavutil-dev \
            libswscale-dev libswresample-dev \
            libcurl4-openssl-dev \
            libx11-dev libxext-dev libxfixes-dev \
            libpulse-dev
    elif command -v dnf &> /dev/null; then
        echo "Using dnf package manager..."
        $SUDO dnf install -y \
            gcc-c++ cmake \
            qt6-qtbase-devel qt6-qtmultimedia-devel \
            opencv-devel \
            ffmpeg-devel \
            libcurl-devel \
            libX11-devel libXext-devel libXfixes-devel \
            pulseaudio-libs-devel
    else
        echo "WARNING: Unknown package manager. Please install dependencies manually."
    fi
    
elif [ "$MACHINE" == "Mac" ]; then
    echo "Installing macOS dependencies..."
    echo ""
    
    # Check if Homebrew is installed
    if ! command -v brew &> /dev/null; then
        echo "ERROR: Homebrew not found. Please install it from https://brew.sh"
        exit 1
    fi
    
    brew install cmake qt6 opencv ffmpeg curl
    
    # Set Qt path
    export CMAKE_PREFIX_PATH="$(brew --prefix qt6)"
    
else
    echo "ERROR: Unsupported operating system"
    exit 1
fi

echo ""
echo "Creating build directory..."
mkdir -p build
cd build

echo ""
echo "Configuring project..."
if [ "$MACHINE" == "Mac" ]; then
    cmake .. -DCMAKE_PREFIX_PATH="$(brew --prefix qt6)"
else
    cmake ..
fi

if [ $? -ne 0 ]; then
    echo ""
    echo "ERROR: CMake configuration failed"
    exit 1
fi

echo ""
echo "Building project..."
if [ "$MACHINE" == "Mac" ]; then
    make -j$(sysctl -n hw.ncpu)
else
    make -j$(nproc)
fi

if [ $? -ne 0 ]; then
    echo ""
    echo "ERROR: Build failed"
    exit 1
fi

cd ..

echo ""
echo "========================================"
echo "Build completed successfully!"
echo "========================================"
echo ""
echo "Executable location: build/ScreenClipRecorder"
echo ""
echo "To run the application:"
echo "  cd build"
echo "  ./ScreenClipRecorder"
echo ""

# Make the executable... executable
chmod +x build/ScreenClipRecorder

echo "You can also run it directly with:"
echo "  ./build/ScreenClipRecorder"
echo ""