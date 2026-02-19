#ifndef SCREENRECORDER_H
#define SCREENRECORDER_H

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QDateTime>
#include <deque>
#include <memory>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#elif __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <CoreVideo/CoreVideo.h>
#else
#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#endif

struct VideoFrame {
    QByteArray jpegData; 
    QSize originalSize;
    QImage::Format format;
    QDateTime timestamp;
};

class ScreenRecorder : public QThread {
    Q_OBJECT

public:
    explicit ScreenRecorder(int fps = 30, QObject *parent = nullptr);
    ~ScreenRecorder();

    void startRecording();
    void stopRecording();
    bool isRecording() const { return m_recording.load(); }
    
    void setFPS(int fps) { m_fps = fps; }
    int getFPS() const { return m_fps; }
    
    void setBufferSeconds(int seconds);
    int getBufferSeconds() const { return m_bufferSeconds; }
    
    // Get frames from buffer
    std::vector<VideoFrame> getFrames(int seconds);
    void clearBuffer();

signals:
    void errorOccurred(const QString& error);
    void recordingStarted();
    void recordingStopped();
    void debugLog(const QString& message);  // Send logs to UI

protected:
    void run() override;

private:
    /*
     * Frame compression helper
     * - `compressFrame` converts a raw captured `QImage` into the on-disk
     *   representation used by the rest of the system (compressed JPEG
     *   stored in `VideoFrame::jpegData`) and records metadata.
     * - Keeping compression as a separate private method centralizes the
     *   behavior and simplifies unit testing and fallback strategies.
     */
    void compressFrame(const QImage& raw, VideoFrame& outFrame);

    int m_fps;
    int m_bufferSeconds;
    std::atomic<bool> m_recording;
    std::atomic<bool> m_stopRequested;
    
    std::deque<VideoFrame> m_frameBuffer;
    QMutex m_bufferMutex;
    
#ifdef _WIN32
    ID3D11Device* m_d3dDevice;
    ID3D11DeviceContext* m_d3dContext;
    IDXGIOutputDuplication* m_deskDupl;
    LONGLONG m_lastFramePresented;
    
    bool initD3D();
    void cleanupD3D();
    bool captureFrameD3D(QImage& outImage);
#elif __APPLE__
    CGDirectDisplayID m_displayID;
    
    bool captureFrameCG(QImage& outImage);
#else
    Display* m_display;
    Window m_root;
    XImage* m_image;
    
    bool initX11();
    void cleanupX11();
    bool captureFrameX11(QImage& outImage);
#endif
};

#endif // SCREENRECORDER_H