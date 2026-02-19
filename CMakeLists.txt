cmake_minimum_required(VERSION 3.16)
project(ScreenClipRecorder VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find required packages
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia Network)
find_package(OpenCV REQUIRED)
find_package(CURL REQUIRED)

# Find FFmpeg - handle both pkg-config and vcpkg
if(WIN32)
    # On Windows with vcpkg, use find_package
    find_package(FFMPEG REQUIRED)
    set(FFMPEG_LIBRARIES
        ${FFMPEG_LIBAVCODEC_LIBRARIES}
        ${FFMPEG_LIBAVFORMAT_LIBRARIES}
        ${FFMPEG_LIBAVUTIL_LIBRARIES}
        ${FFMPEG_LIBSWSCALE_LIBRARIES}
        ${FFMPEG_LIBSWRESAMPLE_LIBRARIES}
    )
    set(FFMPEG_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIRS})
else()
    # On Unix, use pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED
        libavcodec
        libavformat
        libavutil
        libswscale
        libswresample
    )
endif()

# Platform-specific settings
if(WIN32)
    # Windows-specific libraries
    set(PLATFORM_LIBS
        mfplat
        mfreadwrite
        mfuuid
        wmcodecdspuuid
        ole32
        avrt
        ksuser
    )
elseif(APPLE)
    # macOS-specific frameworks
    find_library(COREAUDIO_FRAMEWORK CoreAudio)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox)
    find_library(COREVIDEO_FRAMEWORK CoreVideo)
    find_library(COREMEDIA_FRAMEWORK CoreMedia)
    find_library(AVFOUNDATION_FRAMEWORK AVFoundation)
    find_library(SCREENCAPTUREKIT_FRAMEWORK ScreenCaptureKit)
    
    set(PLATFORM_LIBS
        ${COREAUDIO_FRAMEWORK}
        ${AUDIOTOOLBOX_FRAMEWORK}
        ${COREVIDEO_FRAMEWORK}
        ${COREMEDIA_FRAMEWORK}
        ${AVFOUNDATION_FRAMEWORK}
        ${SCREENCAPTUREKIT_FRAMEWORK}
    )
else()
    # Linux-specific libraries
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PULSEAUDIO REQUIRED libpulse)
    pkg_check_modules(X11 REQUIRED x11 xext xfixes)
    
    set(PLATFORM_LIBS
        ${PULSEAUDIO_LIBRARIES}
        ${X11_LIBRARIES}
        pthread
    )
    
    include_directories(
        ${PULSEAUDIO_INCLUDE_DIRS}
        ${X11_INCLUDE_DIRS}
    )
endif()

# Source files
set(SOURCES
    main.cpp
    MainWindow.cpp
    ScreenRecorder.cpp
    AudioCapture.cpp
    ClipViewer.cpp
    TrimDialog.cpp
    VideoEncoder.cpp
    EncoderWorker.cpp
)

set(HEADERS
    MainWindow.h
    ScreenRecorder.h
    AudioCapture.h
    ClipViewer.h
    TrimDialog.h
    VideoEncoder.h
    EncoderWorker.h 
)

# Create executable
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

# Link libraries
target_link_libraries(${PROJECT_NAME}
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::Network
    ${OpenCV_LIBS}
    ${CURL_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    ${PLATFORM_LIBS}
)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${OpenCV_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
    ${VCODEC_INCLUDE_DIR}
)

# Windows-specific settings
if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        WIN32_EXECUTABLE ON
    )
endif()

# Install target
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)