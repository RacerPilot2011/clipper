/*
 * ScreenRecorder.cpp
 *
 * Cross-platform screen capture helper. On Windows it uses the DirectX
 * Desktop Duplication API (IDXGIOutputDuplication) for efficient, low
 * latency captures. On macOS and Linux the implementation uses the
 * platform-native APIs described in ScreenRecorder.h.
 *
 * The class runs on its own `QThread` and stores compressed frames in a
 * memory-backed circular buffer to support instant-replay functionality.
 */

#include "ScreenRecorder.h"
#include <QDebug>
#include <QThread>
#include <QScreen>
#include <QGuiApplication>
#include <QBuffer>
#include <QIODevice>
#include <algorithm>
#include <d3d11.h>
#include <dxgi1_2.h>

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
    , m_lastFramePresented(0)
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
    // Calculate the maximum number of frames to keep: FPS × desired seconds.
    // If we exceed this limit, discard old frames from the front of the deque.
    size_t maxFrames = seconds * m_fps;
    
    while (m_frameBuffer.size() > maxFrames) {
        m_frameBuffer.pop_front();
    }
}

void ScreenRecorder::startRecording() {
    if (m_recording.load()) {
        emit debugLog("⚠️  WARNING: Recording already in progress");
        return;
    }
    
    emit debugLog("🎬 [ScreenRecorder] Attempting to start recording thread...");
    m_stopRequested = false;
    start();
    emit debugLog("✓ [ScreenRecorder] Thread start() called, waiting for run() to execute");
}

void ScreenRecorder::stopRecording() {
    m_stopRequested = true;
    wait(5000);
}

void ScreenRecorder::compressFrame(const QImage &raw, VideoFrame &outFrame) {
    if (raw.isNull()) {
        emit debugLog("❌ [Compress] ERROR: Raw image is null!");
        return;
    }

    // Convert to RGB888 format which JPEG supports well (JPEG doesn't include alpha).
    // This is a simple color space conversion that the QImage class handles internally.
    QImage frameToSave = raw.convertToFormat(QImage::Format_RGB888);

    QBuffer buffer(&outFrame.jpegData);
    if (!buffer.open(QIODevice::WriteOnly)) {
        emit debugLog("❌ [Compress] ERROR: Failed to open QBuffer for writing!");
        return;
    }

    // Attempt JPEG compression at quality 75 (0-100 scale). Higher quality means
    // larger files but less visible artifacts. Qt's save() function uses libjpeg.
    bool saved = frameToSave.save(&buffer, "JPG", 75);

    // Fallback strategy: if JPEG fails (often due to missing image format plugins),
    // try PNG as a second attempt. PNG is always built into Qt.
    if (!saved) {
        emit debugLog("⚠️ [Compress] JPG save failed (Missing plugin?). Trying PNG...");
        buffer.seek(0);
        outFrame.jpegData.clear();
        saved = frameToSave.save(&buffer, "PNG");
        if (saved) {
             emit debugLog("✓ [Compress] Saved as PNG instead.");
        }
    }

    if (!saved) {
        // If both JPEG and PNG failed, there is a serious issue with image codec
        // plugins or the buffer state. Log diagnostics to help debug.
        emit debugLog("❌ [Compress] CRITICAL: Failed to save image (Both JPG and PNG failed)!");
        emit debugLog(QString("  Input format: %1").arg(raw.format()));
    } else {
        // Log compression success only once to avoid flooding the debug console.
        static bool firstLog = true;
        if (firstLog) {
            emit debugLog("✓ [Compress] Frame compression working!");
            emit debugLog(QString("  Compressed to: %1 KB").arg(outFrame.jpegData.size() / 1024));
            firstLog = false;
        }
    }

    outFrame.originalSize = raw.size();
    outFrame.format = raw.format();
    buffer.close();
}

std::vector<VideoFrame> ScreenRecorder::getFrames(int seconds) {
    QMutexLocker locker(&m_bufferMutex);
    
    emit debugLog("═══════════════════════════════════════════════════════════════");
    emit debugLog("[GetFrames] 🔍 Extracting frames from buffer for encoding");
    emit debugLog(QString("  • Requested duration: %1 seconds").arg(seconds));
    emit debugLog(QString("  • Current buffer size: %1 frames").arg(m_frameBuffer.size()));
    emit debugLog(QString("  • FPS: %1").arg(m_fps));
    
    int framesToGet = seconds * m_fps;
    emit debugLog(QString("  • Frames to retrieve: %1").arg(framesToGet));
    
    std::vector<VideoFrame> result;
    
    int startIdx = (std::max)(0, (int)m_frameBuffer.size() - framesToGet);
    emit debugLog(QString("  • Retrieving from index: %1 to %2").arg(startIdx).arg(m_frameBuffer.size()));
    
    for (size_t i = startIdx; i < m_frameBuffer.size(); i++) {
        result.push_back(m_frameBuffer[i]);
    }
    
    emit debugLog(QString("✓ Retrieved: %1 frames (%.1f seconds)").arg(result.size()).arg(result.size() / (double)m_fps));
    
    // Log compression stats and diagnose empty jpegData
    if (!result.empty()) {
        if (!result[0].jpegData.isEmpty()) {
            emit debugLog("✓✓ Frames ARE properly JPEG compressed (ready for encoding!)");
            emit debugLog(QString("  • Sample frame: %1 KB").arg(result[0].jpegData.size() / 1024));
            emit debugLog(QString("  • Resolution: %1x%2").arg(result[0].originalSize.width()).arg(result[0].originalSize.height()));
            // Calculate total
            size_t totalSize = 0;
            for (const auto& frame : result) {
                totalSize += frame.jpegData.size();
            }
            emit debugLog(QString("  • Total buffer: %1 MB").arg(totalSize / 1024.0 / 1024.0, 0, 'f', 2));
        } else {
            emit debugLog("❌ ERROR: First frame has EMPTY jpegData!");
            emit debugLog(QString("  • Original size stored: %1x%2").arg(result[0].originalSize.width()).arg(result[0].originalSize.height()));
            emit debugLog("  • This means compression never happened!");
        }
    } else {
        emit debugLog("❌ CRITICAL ERROR: NO FRAMES IN BUFFER AT ALL!");
        emit debugLog("  This means either:");
        emit debugLog("    1. Recording thread never started");
        emit debugLog("    2. Frame capture is failing");
        emit debugLog("    3. Buffer was cleared");
    }
    emit debugLog("═══════════════════════════════════════════════════════════════");

    return result;
}

void ScreenRecorder::clearBuffer() {
    QMutexLocker locker(&m_bufferMutex);
    m_frameBuffer.clear();
}

void ScreenRecorder::run() {
#ifdef _WIN32
    if (!initD3D()) {
        emit debugLog("❌ [Init] FAILED: DirectX initialization failed!");
        emit errorOccurred("Failed to initialize DirectX");
        return;
    }
    emit debugLog("✓ [Init] DirectX initialized and ready");
#elif defined(__linux__)
    if (!initX11()) {
        emit debugLog("❌ [Init] FAILED: X11 initialization failed!");
        emit errorOccurred("Failed to initialize X11");
        return;
    }
    emit debugLog("✓ [Init] X11 initialized and ready");
#else
    emit debugLog("✓ [Init] macOS native capture ready");
#endif

    m_recording = true;
    emit recordingStarted();

    emit debugLog("═══════════════════════════════════════════════════════════════");
    emit debugLog("✓ [Recording] STARTED - Capture loop is now active");
    emit debugLog(QString("  • FPS: %1").arg(m_fps));
    emit debugLog(QString("  • Buffer duration: %1 seconds").arg(m_bufferSeconds));
    emit debugLog(QString("  • Max frames in buffer: %1").arg(m_bufferSeconds * m_fps));
    emit debugLog("═══════════════════════════════════════════════════════════════");

    qint64 frameDelay = 1000 / m_fps;
    size_t maxFrames = m_bufferSeconds * m_fps;

    int frameCount = 0;
    int failureCount = 0;
    int consecutiveFailures = 0;
    size_t totalCompressedSize = 0;

    // Reuse this object to prevent heap fragmentation
    QImage rawFrame;

    while (!m_stopRequested.load()) {
        QDateTime startTime = QDateTime::currentDateTime();
        bool success = false;

#ifdef _WIN32
        success = captureFrameD3D(rawFrame);
#elif __APPLE__
        success = captureFrameCG(rawFrame);
#else
        success = captureFrameX11(rawFrame);
#endif

        if (success && !rawFrame.isNull()) {
            consecutiveFailures = 0; // Reset on success
            failureCount = 0; // Reset total failure count on success

            VideoFrame vf;
            vf.timestamp = startTime;

            // Sanity-check that the captured frame has valid dimensions before compression.
            // Invalid dimensions indicate a capture failure at the OS level.
            if (rawFrame.size().width() <= 0 || rawFrame.size().height() <= 0) {
                emit debugLog(QString("❌ [Capture] ERROR: Invalid frame dimensions: %1x%2")
                    .arg(rawFrame.size().width()).arg(rawFrame.size().height()));
            } else {
                // Compress the raw frame into JPEG format for memory efficiency.
                compressFrame(rawFrame, vf);

                // Verify that compression succeeded and produced non-empty data.
                if (vf.jpegData.isEmpty()) {
                    emit debugLog("❌ [Compression] ERROR: Compression failed - jpegData is empty!");
                    emit debugLog(QString("  Raw frame size: %1x%2")
                        .arg(rawFrame.size().width()).arg(rawFrame.size().height()));
                } else {
                    // Buffer the compressed frame and apply time-based pruning.
                    {
                        QMutexLocker locker(&m_bufferMutex);
                        m_frameBuffer.push_back(vf);

                        totalCompressedSize += vf.jpegData.size();

                        while (m_frameBuffer.size() > maxFrames) {
                            totalCompressedSize -= m_frameBuffer.front().jpegData.size();
                            m_frameBuffer.pop_front();
                        }
                    }

                    frameCount++;

                    // Log first successful frame
                    if (frameCount == 1) {
                        emit debugLog("═══════════════════════════════════════════════════════════════");
                        emit debugLog("✓✓✓ [Capture] FIRST FRAME CAPTURED & BUFFERED SUCCESSFULLY ✓✓✓");
                        emit debugLog(QString("  • Frame size: %1 KB (compressed)").arg(vf.jpegData.size() / 1024));
                        emit debugLog(QString("  • Resolution: %1x%2")
                            .arg(vf.originalSize.width()).arg(vf.originalSize.height()));
                        emit debugLog("  • System is now recording frames into buffer at " +
                            QString::number(m_fps) + " FPS");
                        emit debugLog("═══════════════════════════════════════════════════════════════");
                    }

                    // Log stats every 10 seconds
                    if (frameCount % (m_fps * 10) == 0) {
                        double avgSize = totalCompressedSize / (double)m_frameBuffer.size();
                        double totalMB = totalCompressedSize / 1024.0 / 1024.0;
                        double duration = m_frameBuffer.size() / (double)m_fps;
                        emit debugLog(QString("[Stats] ✓ %1 frames (%2 sec) | Avg: %3 KB | Total: %4 MB")
                            .arg(frameCount)
                            .arg(duration, 0, 'f', 1)
                            .arg(avgSize / 1024, 0, 'f', 1)
                            .arg(totalMB, 0, 'f', 2));
                    }
                }
            }
        } else {
            // Failure handling
            consecutiveFailures++;
            failureCount++;
            
            // Log early failures for debugging
            if (frameCount == 0 && failureCount <= 5) {
                emit debugLog(QString("⚠️  [Capture] Attempt %1 failed (success=%2, isNull=%3)")
                    .arg(failureCount).arg(success).arg(rawFrame.isNull()));
            }

            // Attempt recovery after sustained failures
            if (consecutiveFailures >= 50) { // ~5 seconds at 10fps
                emit debugLog(QString("🔄 [Recovery] %1 consecutive failures - attempting D3D recovery...")
                    .arg(consecutiveFailures));

                // CRITICAL: Must fully cleanup before reinit
                cleanupD3D();
                QThread::msleep(500); // Longer delay to ensure cleanup
                
                if (initD3D()) {
                    emit debugLog("✓ [Recovery] D3D reinitialized successfully!");
                    consecutiveFailures = 0;
                    failureCount = 0;
                } else {
                    emit debugLog("❌ [Recovery] D3D reinit failed - stopping recording");
                    emit errorOccurred("DirectX recovery failed");
                    break;
                }
            }

            // Give up if we never get started
            if (frameCount == 0 && failureCount >= 100) {
                emit debugLog("❌ [CRITICAL] Unable to capture ANY frames after 100 attempts!");
                emit debugLog("  Possible causes:");
                emit debugLog("    - Screen content isn't updating (move mouse to test)");
                emit debugLog("    - Another app is using Desktop Duplication");
                emit debugLog("    - GPU driver issue");
                emit errorOccurred("Failed to capture any frames - check for conflicting apps");
                break;
            }
        }

        // Accurate sleep timing
        qint64 elapsed = startTime.msecsTo(QDateTime::currentDateTime());
        qint64 sleepTime = frameDelay - elapsed;
        if (sleepTime > 0) {
            QThread::msleep(sleepTime);
        }
    }

    emit debugLog("═══════════════════════════════════════════════════════════════");
    emit debugLog("⏹️  [Recording] STOPPED - Capture loop terminated");
    emit debugLog(QString("  • Total frames captured: %1").arg(frameCount));
    emit debugLog(QString("  • Final buffer size: %1 frames").arg(m_frameBuffer.size()));
    double finalBufferDuration = m_frameBuffer.size() / (double)m_fps;
    emit debugLog(QString("  • Buffer duration: %1 seconds").arg(finalBufferDuration, 0, 'f', 1));
    emit debugLog("═══════════════════════════════════════════════════════════════");

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
    
    emit debugLog("═══════════════════════════════════════════════════════════════");
    emit debugLog("[D3D Init] Starting DirectX Desktop Duplication initialization");
    emit debugLog(QString("  User: %1").arg(qEnvironmentVariable("USERNAME")));
    
    // Check for Remote Desktop
    bool isRemote = GetSystemMetrics(SM_REMOTESESSION);
    emit debugLog(QString("  Remote Session: %1").arg(isRemote ? "YES - WILL FAIL" : "No"));
    
    if (isRemote) {
        emit debugLog("❌ [D3D Init] Desktop Duplication does NOT work over Remote Desktop!");
        return false;
    }
    
    // Check for conflicting software
    emit debugLog("  NOTE: Close these if running:");
    emit debugLog("    - OBS Studio, Streamlabs");
    emit debugLog("    - Discord (disable overlay)");
    emit debugLog("    - AMD Software overlay");
    emit debugLog("    - GeForce Experience overlay");
    emit debugLog("    - Xbox Game Bar (Win+G)");
    emit debugLog("═══════════════════════════════════════════════════════════════");
    
    IDXGIFactory1* factory = nullptr;
    hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    if (FAILED(hr)) {
        emit debugLog(QString("❌ [D3D Init] CreateDXGIFactory1 failed: 0x%1").arg(hr, 0, 16));
        return false;
    }

    UINT i = 0;
    IDXGIAdapter1* adapter = nullptr;
    IDXGIAdapter1* selectedAdapter = nullptr;
    IDXGIOutput1* selectedOutput = nullptr;
    bool found = false;

    // Enumerate adapters and outputs
    while (factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        QString adapterName = QString::fromWCharArray(desc.Description);
        
        emit debugLog(QString("[D3D Init] Adapter %1: %2").arg(i).arg(adapterName));
        emit debugLog(QString("  VRAM: %1 MB").arg(desc.DedicatedVideoMemory / 1024 / 1024));

        // Skip software/remote adapters
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            emit debugLog("  -> Skipping (software adapter)");
            adapter->Release();
            i++;
            continue;
        }

        UINT j = 0;
        IDXGIOutput* output = nullptr;
        while (adapter->EnumOutputs(j, &output) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC outDesc;
            output->GetDesc(&outDesc);
            QString outputName = QString::fromWCharArray(outDesc.DeviceName);
            
            emit debugLog(QString("  Output %1: %2 (Attached: %3)")
                .arg(j).arg(outputName).arg(outDesc.AttachedToDesktop ? "Yes" : "No"));
            
            // Only use attached outputs
            if (!outDesc.AttachedToDesktop) {
                output->Release();
                j++;
                continue;
            }
            
            IDXGIOutput1* output1 = nullptr;
            hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
            output->Release();

            if (SUCCEEDED(hr)) {
                // Try to create D3D device
                D3D_FEATURE_LEVEL featureLevels[] = {
                    D3D_FEATURE_LEVEL_11_0,
                    D3D_FEATURE_LEVEL_10_1,
                    D3D_FEATURE_LEVEL_10_0
                };
                D3D_FEATURE_LEVEL featureLevel;
                
                UINT createFlags = 0;
#ifdef _DEBUG
                createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
                
                hr = D3D11CreateDevice(
                    adapter, 
                    D3D_DRIVER_TYPE_UNKNOWN, 
                    nullptr, 
                    createFlags, 
                    featureLevels, 
                    ARRAYSIZE(featureLevels), 
                    D3D11_SDK_VERSION, 
                    &m_d3dDevice, 
                    &featureLevel, 
                    &m_d3dContext
                );

                if (SUCCEEDED(hr)) {
                    emit debugLog(QString("  -> D3D Device created (Feature Level: 0x%1)").arg(featureLevel, 0, 16));
                    
                    // Try DuplicateOutput - THIS is where conflicts happen
                    hr = output1->DuplicateOutput(m_d3dDevice, &m_deskDupl);
                    
                    if (SUCCEEDED(hr)) {
                        // Get duplication info
                        DXGI_OUTDUPL_DESC duplDesc;
                        m_deskDupl->GetDesc(&duplDesc);
                        
                        emit debugLog("✓✓✓ [D3D Init] SUCCESS!");
                        emit debugLog(QString("  Resolution: %1x%2")
                            .arg(duplDesc.ModeDesc.Width).arg(duplDesc.ModeDesc.Height));
                        emit debugLog(QString("  Refresh Rate: %1 Hz")
                            .arg(duplDesc.ModeDesc.RefreshRate.Numerator / 
                                 (std::max)(1u, duplDesc.ModeDesc.RefreshRate.Denominator)));
                        emit debugLog(QString("  Rotation: %1").arg(duplDesc.Rotation));
                        emit debugLog("═══════════════════════════════════════════════════════════════");
                        
                        selectedAdapter = adapter;
                        selectedOutput = output1;
                        found = true;
                        m_lastFramePresented = 0;
                        break;
                    } else {
                        // DuplicateOutput failed - explain why
                        QString errorMsg;
                        if (hr == E_ACCESSDENIED) {
                            errorMsg = "E_ACCESSDENIED - Another app is using Desktop Duplication!";
                        } else if (hr == DXGI_ERROR_UNSUPPORTED) {
                            errorMsg = "DXGI_ERROR_UNSUPPORTED - Not supported on this output";
                        } else if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
                            errorMsg = "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE - Try different output";
                        } else if (hr == DXGI_ERROR_SESSION_DISCONNECTED) {
                            errorMsg = "DXGI_ERROR_SESSION_DISCONNECTED - Remote desktop active";
                        } else {
                            errorMsg = QString("Unknown error: 0x%1").arg(hr, 0, 16);
                        }
                        emit debugLog(QString("  -> DuplicateOutput FAILED: %1").arg(errorMsg));
                        
                        // Cleanup failed device
                        if (m_d3dDevice) {
                            m_d3dDevice->Release();
                            m_d3dDevice = nullptr;
                        }
                        if (m_d3dContext) {
                            m_d3dContext->Release();
                            m_d3dContext = nullptr;
                        }
                    }
                } else {
                    emit debugLog(QString("  -> D3D11CreateDevice failed: 0x%1").arg(hr, 0, 16));
                }
                
                if (!found) {
                    output1->Release();
                }
            }
            j++;
        }
        
        if (found) {
            break;
        }
        
        adapter->Release();
        i++;
    }

    factory->Release();

    if (!found) {
        emit debugLog("❌ [D3D Init] FAILED: No compatible adapter/output found!");
        return false;
    }
    
    // Clean up adapter/output references (we don't need to hold them)
    if (selectedAdapter) selectedAdapter->Release();
    if (selectedOutput) selectedOutput->Release();

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
    
    m_lastFramePresented = 0;
}

bool ScreenRecorder::captureFrameD3D(QImage& outImage) {
    if (!m_deskDupl || !m_d3dDevice || !m_d3dContext) {
        return false;
    }
    
    static int consecutiveTimeouts = 0;
    static int totalFramesCaptured = 0;
    
    IDXGIResource* desktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    HRESULT hr = S_OK;
    
    // FIRST ATTEMPT: Try normal capture without forcing
    hr = m_deskDupl->AcquireNextFrame(100, &frameInfo, &desktopResource);
    
    // If timeout AND we've had multiple consecutive timeouts, THEN force update and retry
    if (hr == DXGI_ERROR_WAIT_TIMEOUT && consecutiveTimeouts >= 3) {
        // Force desktop update
        HWND desktopHwnd = GetDesktopWindow();
        InvalidateRect(desktopHwnd, NULL, FALSE);
        UpdateWindow(desktopHwnd);
        QThread::msleep(5);
        
        // Retry with longer timeout
        hr = m_deskDupl->AcquireNextFrame(300, &frameInfo, &desktopResource);
        
        static int forceLogCount = 0;
        if (forceLogCount < 5) {
            emit debugLog(QString("🔄 [AMD] Forced desktop update after %1 timeouts").arg(consecutiveTimeouts));
            forceLogCount++;
        }
    }
    
    // Handle WAIT_TIMEOUT
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        consecutiveTimeouts++;
        
        // Log only occasionally
        if (consecutiveTimeouts == 10 || consecutiveTimeouts == 30 || consecutiveTimeouts % 100 == 0) {
            emit debugLog(QString("⚠️ [AMD] %1 consecutive timeouts (total frames captured: %2)")
                .arg(consecutiveTimeouts).arg(totalFramesCaptured));
        }
        
        return false;
    }
    
    // Handle ACCESS_LOST
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        static int accessLostCount = 0;
        if (accessLostCount < 3) {
            emit debugLog("⚠️ [D3D Capture] DXGI_ERROR_ACCESS_LOST - will attempt recovery");
            accessLostCount++;
        }
        return false;
    }
    
    // Handle other errors
    if (FAILED(hr)) {
        static int errorCount = 0;
        if (errorCount < 5) {
            QString errorMsg;
            if (hr == E_ACCESSDENIED) {
                errorMsg = "E_ACCESSDENIED - Another process took control";
            } else if (hr == DXGI_ERROR_INVALID_CALL) {
                errorMsg = "DXGI_ERROR_INVALID_CALL - Invalid state";
            } else {
                errorMsg = QString("0x%1").arg(hr, 0, 16);
            }
            emit debugLog(QString("❌ [D3D Capture] AcquireNextFrame failed: %1").arg(errorMsg));
            errorCount++;
        }
        return false;
    }
    
    // SUCCESS - reset timeout counter
    consecutiveTimeouts = 0;
    
    // Validate frame info
    if (frameInfo.LastPresentTime.QuadPart == 0) {
        desktopResource->Release();
        m_deskDupl->ReleaseFrame();
        return false;
    }
    
    // Check for duplicate frames
    if (frameInfo.LastPresentTime.QuadPart == m_lastFramePresented) {
        desktopResource->Release();
        m_deskDupl->ReleaseFrame();
        return false;
    }
    
    m_lastFramePresented = frameInfo.LastPresentTime.QuadPart;
    
    // Get texture interface
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
    
    if (desc.Width == 0 || desc.Height == 0) {
        texture->Release();
        m_deskDupl->ReleaseFrame();
        return false;
    }
    
    // Create staging texture for CPU access
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
    
    // Create QImage
    outImage = QImage(desc.Width, desc.Height, QImage::Format_ARGB32);
    
    if (outImage.isNull()) {
        m_d3dContext->Unmap(stagingTexture, 0);
        stagingTexture->Release();
        m_deskDupl->ReleaseFrame();
        return false;
    }
    
    // Copy data with proper pitch handling
    for (UINT y = 0; y < desc.Height; y++) {
        memcpy(outImage.scanLine(y),
               (BYTE*)mapped.pData + (y * mapped.RowPitch),
               desc.Width * 4);
    }
    
    // Cleanup
    m_d3dContext->Unmap(stagingTexture, 0);
    stagingTexture->Release();
    m_deskDupl->ReleaseFrame();
    
    // Track successful captures
    totalFramesCaptured++;
    
    // Log success occasionally
    if (totalFramesCaptured == 1) {
        emit debugLog("✓✓✓ [AMD] First frame captured successfully!");
    } else if (totalFramesCaptured == 10) {
        emit debugLog("✓ [AMD] 10 frames captured - system stable");
    } else if (totalFramesCaptured % 300 == 0) { // Every 10 seconds at 30fps
        emit debugLog(QString("✓ [AMD] %1 frames captured total").arg(totalFramesCaptured));
    }
    
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

    if (image->depth == 24 || image->depth == 32) {
        QImage wrapper(
            (const uchar*)image->data, 
            image->width, 
            image->height, 
            image->bytes_per_line, 
            QImage::Format_RGB32);
        
        outImage = wrapper.copy(); 
    } else {
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
    }

    XDestroyImage(image);
    return true;
}

#endif