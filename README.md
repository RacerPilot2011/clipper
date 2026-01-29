# Screen Clip Recorder - C++ Cross-Platform Edition

A high-performance screen recorder with instant replay functionality.

## Features

- **Windows**: Direct desktop audio capture using WASAPI loopback
- **macOS**: Screen and audio capture using CoreAudio/ScreenCaptureKit
- **Linux**: X11 screen capture with PulseAudio
- Instant replay buffer (15s to 5min configurable)
- Microphone and desktop audio mixing
- Hardware-accelerated screen capture
- H.264 video encoding with AAC audio
- Global hotkeys
- Video trimming and editing
- Cloud upload support

## Platform-Specific Audio Capture

### Windows
- **Desktop Audio**: Uses WASAPI loopback mode - captures ANY audio playing on the system
- **No Stereo Mix required** - Works out of the box
- **Microphone**: DirectShow/WASAPI capture
- Automatically detects and lists all audio devices

### macOS
- **Desktop Audio**: Requires BlackHole or similar virtual audio device
- **Microphone**: CoreAudio capture
- Instructions provided for BlackHole setup

### Linux
- **Desktop Audio**: PulseAudio monitor devices
- **Microphone**: PulseAudio capture
- Works with most modern Linux distributions

## Dependencies

### All Platforms
- CMake 3.16+
- Qt 6.x (Core, Gui, Widgets, Multimedia, Network)
- FFmpeg libraries (libavcodec, libavformat, libavutil, libswscale, libswresample)
- OpenCV 4.x
- libcurl

### Windows-Specific
- Visual Studio 2019/2022 or MinGW
- Windows SDK (for WASAPI, DirectX)

### macOS-Specific
- Xcode Command Line Tools
- macOS 11.0+ (for ScreenCaptureKit)

### Linux-Specific
- GCC 9+ or Clang 10+
- X11 development libraries
- PulseAudio development libraries

## Building

### Windows

```powershell
# Install dependencies via vcpkg
vcpkg install qt6 opencv4 ffmpeg curl

# Build
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkg root]/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release

# Run
.\Release\ScreenClipRecorder.exe
```

### macOS

```bash
# Install dependencies via Homebrew
brew install cmake qt6 opencv ffmpeg curl

# Build
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=$(brew --prefix qt6)
make -j$(sysctl -n hw.ncpu)

# Run
./ScreenClipRecorder
```

### Linux (Ubuntu/Debian)

```bash
# Install dependencies
sudo apt update
sudo apt install -y \
    build-essential cmake \
    qt6-base-dev qt6-multimedia-dev \
    libopencv-dev \
    libavcodec-dev libavformat-dev libavutil-dev \
    libswscale-dev libswresample-dev \
    libcurl4-openssl-dev \
    libx11-dev libxext-dev libxfixes-dev \
    libpulse-dev

# Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Run
./ScreenClipRecorder
```

## Usage

### First Time Setup

**Windows**: 
- No setup required! Desktop audio works immediately via WASAPI loopback
- Just select your speakers/headphones from the "Desktop Audio" dropdown

**macOS**:
- For desktop audio, install [BlackHole](https://existential.audio/blackhole/)
- Create a Multi-Output Device in Audio MIDI Setup
- Select BlackHole in the app's Desktop Audio dropdown

**Linux**:
- Desktop audio uses PulseAudio monitor devices
- Select "Monitor of [your device]" from the Desktop Audio dropdown

### Recording

1. Launch the application
2. Recording starts automatically with a 30-second buffer
3. Select audio devices:
   - **Microphone**: Your physical microphone
   - **Desktop Audio**: System audio output (Windows: any device, macOS/Linux: requires setup)
4. Click "Apply" after selecting devices
5. Press **F9** (or click "Save Clip") to save the last 30 seconds
6. Clips are saved to `~/ScreenClips/`

### Customization

- **Buffer Duration**: Change replay buffer from 15 seconds to 5 minutes
- **Hotkey**: Customize the save hotkey (default F9)
- **FPS**: Adjust frame rate for quality vs file size
- **Upload**: Set username for cloud uploads

### Advanced Features

- **Trimming**: Select a clip and click "Trim" to cut unwanted parts
- **Renaming**: Give clips custom names for organization
- **Preview**: Built-in video player for instant playback
- **Upload**: Share clips to cloud with a single click

## Technical Details

### Architecture

```
┌─────────────────┐
│   MainWindow    │  Qt GUI
└────────┬────────┘
         │
    ┌────┴────┬──────────┬────────────┐
    │         │          │            │
┌───▼──┐  ┌──▼──┐  ┌────▼─────┐  ┌──▼──────┐
│Screen│  │Audio│  │   Audio  │  │  Video  │
│Record│  │Mic  │  │ Desktop  │  │ Encoder │
└──────┘  └─────┘  └──────────┘  └─────────┘
    │         │          │            │
    │    Circular Buffers (Deque)     │
    │         │          │            │
    └────────┴──────────┴────────────┘
                 │
          ┌──────▼───────┐
          │ Save to MP4  │
          │  (H264+AAC)  │
          └──────────────┘
```

### Windows WASAPI Loopback Explained

Traditional Python approaches require "Stereo Mix" (often disabled) or virtual cables. This C++ implementation uses **WASAPI loopback mode**:

1. Opens the actual speakers/headphones device
2. Sets `AUDCLNT_STREAMFLAGS_LOOPBACK` flag
3. Captures the mixed audio stream before it hits the hardware
4. Works with ANY output device, no configuration needed

This is the same technique used by professional tools like OBS Studio and ShareX.

### Performance

- **Screen Capture**: 
  - Windows: DirectX Desktop Duplication (~5% CPU @ 30fps)
  - macOS: CoreGraphics (~8% CPU @ 30fps)
  - Linux: X11 XGetImage (~12% CPU @ 30fps)

- **Memory**: ~200MB for 30s buffer @ 1080p 30fps
- **Disk**: ~50MB per 30s clip (H264 CRF 23)

## Troubleshooting

### Windows: "Failed to initialize audio client"
- Make sure no other app has exclusive access to the audio device
- Try a different desktop audio device
- Restart the application

### macOS: "Desktop audio not capturing"
- Install BlackHole 2ch
- Create Multi-Output Device in Audio MIDI Setup
- Set system output to Multi-Output Device
- Select BlackHole in the app

### Linux: "No monitor devices found"
- Run: `pactl list sources` to see available sources
- Look for sources with ".monitor" in the name
- Make sure PulseAudio is running

### All Platforms: "FFmpeg encoding failed"
- Ensure FFmpeg libraries are properly installed
- Check that all .dll/.so/.dylib files are in the path
- Try reducing video resolution or bitrate

## License

MIT License - See LICENSE file for details

## Contributing

Contributions welcome! Please:
1. Fork the repository
2. Create a feature branch
3. Test on your platform
4. Submit a pull request

## Acknowledgments

- Qt Framework for cross-platform GUI
- FFmpeg for video encoding
- OpenCV for image processing
- WASAPI/CoreAudio/PulseAudio for audio capture

## Roadmap

- [ ] GPU-accelerated encoding (NVENC, VideoToolbox, VAAPI)
- [ ] Region selection for recording
- [ ] Webcam overlay
- [ ] Live streaming support
- [ ] Plugin system for effects
- [ ] Timeline editor for multi-clip editing