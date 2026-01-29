#include "ScreenRecorder.h"
#include <QDebug>
#include <QThread>
#include <QScreen>
#include <QGuiApplication>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#endif

ScreenRecorder::ScreenRecorder(int fps, QObject *parent)
    : QThread(parent)
    , m_fps(fps)
    , m_bufferSeconds(30)
    , m_recording(false)
    , m_stopRequested(false)
#ifdef _WIN32
    , m_d3dDevice(nullptr)
    , m_d3dContext(nullptr)
    , m_deskDupl(nullptr)
#elif __APPLE__
    , m_displayID(CGMainDisplayID())
#else
    , m_display(nullptr)
    , m_root(0)
    , m_image(nullptr)
#endif
{
}

ScreenRecorder::~ScreenRecorder() {
    stopRecording();
#ifdef _WIN32
    cleanupD3D();
#elif defined(__linux__)
    cleanupX11();
#endif
}

void ScreenRecorder::setBufferSeconds(int seconds) {
    m_bufferSeconds = seconds;
    
    QMutexLocker locker(&m_bufferMutex);
    size_t maxFrames = seconds * m_fps;
    
    while (m_frameBuffer.size() > maxFrames) {
        m_frameBuffer.pop_front();
    }
}

void ScreenRecorder::startRecording() {
    if (m_recording.load()) {
        return;
    }
    
    m_stopRequested = false;
    start();
}

void ScreenRecorder::stopRecording() {
    m_stopRequested = true;
    wait(5000);
}

std::vector<VideoFrame> ScreenRecorder::getFrames(int seconds) {
    QMutexLocker locker(&m_bufferMutex);
    
    int framesToGet = seconds * m_fps;
    std::vector<VideoFrame> result;
    
    int startIdx = (std::max)(0, (int)m_frameBuffer.size() - framesToGet);
    for (size_t i = startIdx; i < m_frameBuffer.size(); i++) {
        result.push_back(m_frameBuffer[i]);
    }
    
    return result;
}

void ScreenRecorder::clearBuffer() {
    QMutexLocker locker(&m_bufferMutex);
    m_frameBuffer.clear();
}

void ScreenRecorder::run() {
#ifdef _WIN32
    if (!initD3D()) {
        emit errorOccurred("Failed to initialize DirectX");
        return;
    }
#elif defined(__linux__)
    if (!initX11()) {
        emit errorOccurred("Failed to initialize X11");
        return;
    }
#endif
    
    m_recording = true;
    emit recordingStarted();
    
    qint64 frameDelay = 1000 / m_fps; // milliseconds
    size_t maxFrames = m_bufferSeconds * m_fps;
    
    while (!m_stopRequested.load()) {
        QDateTime startTime = QDateTime::currentDateTime();
        
        QImage frame;
        bool success = false;
        
#ifdef _WIN32
        success = captureFrameD3D(frame);
#elif __APPLE__
        success = captureFrameCG(frame);
#else
        success = captureFrameX11(frame);
#endif
        
        if (success && !frame.isNull()) {
            VideoFrame vf;
            vf.image = frame;
            vf.timestamp = startTime;
            
            QMutexLocker locker(&m_bufferMutex);
            m_frameBuffer.push_back(vf);
            
            while (m_frameBuffer.size() > maxFrames) {
                m_frameBuffer.pop_front();
            }
        }
        
        // Maintain target FPS
        qint64 elapsed = startTime.msecsTo(QDateTime::currentDateTime());
        qint64 sleepTime = frameDelay - elapsed;
        
        if (sleepTime > 0) {
            QThread::msleep(sleepTime);
        }
    }
    
#ifdef _WIN32
    cleanupD3D();
#elif defined(__linux__)
    cleanupX11();
#endif
    
    m_recording = false;
    emit recordingStopped();
}

#ifdef _WIN32

bool ScreenRecorder::initD3D() {
    HRESULT hr;
    
    // Create D3D11 Device
    D3D_FEATURE_LEVEL featureLevel;
    hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &m_d3dDevice,
        &featureLevel,
        &m_d3dContext);
    
    if (FAILED(hr)) {
        qDebug() << "Failed to create D3D11 device:" << hr;
        return false;
    }
    
    // Get DXGI device
    IDXGIDevice* dxgiDevice = nullptr;
    hr = m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr)) {
        qDebug() << "Failed to get DXGI device";
        return false;
    }
    
    // Get DXGI adapter
    IDXGIAdapter* dxgiAdapter = nullptr;
    hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&dxgiAdapter);
    dxgiDevice->Release();
    
    if (FAILED(hr)) {
        qDebug() << "Failed to get DXGI adapter";
        return false;
    }
    
    // Get output
    IDXGIOutput* dxgiOutput = nullptr;
    hr = dxgiAdapter->EnumOutputs(0, &dxgiOutput);
    dxgiAdapter->Release();
    
    if (FAILED(hr)) {
        qDebug() << "Failed to enumerate outputs";
        return false;
    }
    
    // Get output1
    IDXGIOutput1* dxgiOutput1 = nullptr;
    hr = dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&dxgiOutput1);
    dxgiOutput->Release();
    
    if (FAILED(hr)) {
        qDebug() << "Failed to get output1";
        return false;
    }
    
    // Create desktop duplication
    hr = dxgiOutput1->DuplicateOutput(m_d3dDevice, &m_deskDupl);
    dxgiOutput1->Release();
    
    if (FAILED(hr)) {
        qDebug() << "Failed to create desktop duplication:" << hr;
        return false;
    }
    
    return true;
}

void ScreenRecorder::cleanupD3D() {
    if (m_deskDupl) {
        m_deskDupl->Release();
        m_deskDupl = nullptr;
    }
    
    if (m_d3dContext) {
        m_d3dContext->Release();
        m_d3dContext = nullptr;
    }
    
    if (m_d3dDevice) {
        m_d3dDevice->Release();
        m_d3dDevice = nullptr;
    }
}

bool ScreenRecorder::captureFrameD3D(QImage& outImage) {
    if (!m_deskDupl) {
        return false;
    }
    
    IDXGIResource* desktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    
    HRESULT hr = m_deskDupl->AcquireNextFrame(100, &frameInfo, &desktopResource);
    
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;
    }
    
    if (FAILED(hr)) {
        // Try to recreate duplication
        m_deskDupl->Release();
        m_deskDupl = nullptr;
        initD3D();
        return false;
    }
    
    // Get texture
    ID3D11Texture2D* texture = nullptr;
    hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture);
    desktopResource->Release();
    
    if (FAILED(hr)) {
        m_deskDupl->ReleaseFrame();
        return false;
    }
    
    // Get texture description
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    
    // Create staging texture
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    
    ID3D11Texture2D* stagingTexture = nullptr;
    hr = m_d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    
    if (FAILED(hr)) {
        texture->Release();
        m_deskDupl->ReleaseFrame();
        return false;
    }
    
    // Copy to staging texture
    m_d3dContext->CopyResource(stagingTexture, texture);
    texture->Release();
    
    // Map staging texture
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = m_d3dContext->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    
    if (FAILED(hr)) {
        stagingTexture->Release();
        m_deskDupl->ReleaseFrame();
        return false;
    }
    
    // Copy to QImage
    outImage = QImage(desc.Width, desc.Height, QImage::Format_RGB32);
    
    for (UINT y = 0; y < desc.Height; y++) {
        memcpy(outImage.scanLine(y),
               (BYTE*)mapped.pData + (y * mapped.RowPitch),
               desc.Width * 4);
    }
    
    m_d3dContext->Unmap(stagingTexture, 0);
    stagingTexture->Release();
    m_deskDupl->ReleaseFrame();
    
    return true;
}

#elif __APPLE__

bool ScreenRecorder::captureFrameCG(QImage& outImage) {
    CGImageRef screenshot = CGDisplayCreateImage(m_displayID);
    
    if (!screenshot) {
        return false;
    }
    
    size_t width = CGImageGetWidth(screenshot);
    size_t height = CGImageGetHeight(screenshot);
    
    outImage = QImage(width, height, QImage::Format_RGB32);
    
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(
        outImage.bits(),
        width,
        height,
        8,
        outImage.bytesPerLine(),
        colorSpace,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), screenshot);
    
    CGContextRelease(context);
    CGColorSpaceRelease(colorSpace);
    CGImageRelease(screenshot);
    
    return true;
}

#else

bool ScreenRecorder::initX11() {
    m_display = XOpenDisplay(nullptr);
    if (!m_display) {
        return false;
    }
    
    m_root = DefaultRootWindow(m_display);
    return true;
}

void ScreenRecorder::cleanupX11() {
    if (m_image) {
        XDestroyImage(m_image);
        m_image = nullptr;
    }
    
    if (m_display) {
        XCloseDisplay(m_display);
        m_display = nullptr;
    }
}

bool ScreenRecorder::captureFrameX11(QImage& outImage) {
    if (!m_display) {
        return false;
    }
    
    XWindowAttributes attrs;
    XGetWindowAttributes(m_display, m_root, &attrs);
    
    XImage* image = XGetImage(
        m_display,
        m_root,
        0, 0,
        attrs.width,
        attrs.height,
        AllPlanes,
        ZPixmap);
    
    if (!image) {
        return false;
    }
    
    outImage = QImage(attrs.width, attrs.height, QImage::Format_RGB32);
    
    for (int y = 0; y < attrs.height; y++) {
        for (int x = 0; x < attrs.width; x++) {
            unsigned long pixel = XGetPixel(image, x, y);
            
            int r = (pixel & image->red_mask) >> 16;
            int g = (pixel & image->green_mask) >> 8;
            int b = (pixel & image->blue_mask);
            
            outImage.setPixel(x, y, qRgb(r, g, b));
        }
    }
    
    XDestroyImage(image);
    
    return true;
}

#endif