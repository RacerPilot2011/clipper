#ifndef AUDIOCAPTURE_H
#define AUDIOCAPTURE_H

#include <QObject>
#include <QMutex>
#include <QThread>
#include <vector>
#include <deque>
#include <memory>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#elif __APPLE__
#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>
#else
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#endif

struct AudioSample {
    std::vector<float> data;
    int channels;
    int sampleRate;
    double timestamp;
};

class AudioCapture : public QThread {
    Q_OBJECT

public:
    enum DeviceType {
        Microphone,
        DesktopAudio
    };

    explicit AudioCapture(DeviceType type, QObject *parent = nullptr);
    ~AudioCapture();

    /*
     * Device enumeration
     * - `getAvailableDevices` returns a list of available audio endpoints for
     *   the requested `DeviceType` (Microphone or DesktopAudio). The returned
     *   strings are "display name|device id" so the UI can show a human
     *   friendly name and store the exact id required by the platform API.
     */
    static QStringList getAvailableDevices(DeviceType type);

    /*
     * Device selection
     * - `setDevice` selects which device (by id) will be used when capture
     *   is started. The call fails if the capture thread is active.
     * - `currentDevice` returns the currently selected id (empty if none).
     */
    bool setDevice(const QString& deviceId);
    QString currentDevice() const { return m_deviceId; }

    /*
     * Control
     * - `startCapture()` requests that the background capture thread start.
     * - `stopCapture()` requests a stop and waits briefly for the thread to
     *   finish. These methods are thread-safe from the GUI thread.
     */
    void startCapture();
    void stopCapture();
    bool isCapturing() const { return m_capturing.load(); }

    /*
     * Buffer access
     * - `getBuffer(seconds)` returns up to `seconds` of recent audio chunks.
     * - `clearBuffer()` removes all buffered audio immediately.
     */
    std::vector<AudioSample> getBuffer(int seconds);
    void clearBuffer();

signals:
    void errorOccurred(const QString& error);
    void captureStarted();
    void captureStopped();

protected:
    void run() override;

private:
    DeviceType m_deviceType;
    QString m_deviceId;
    std::atomic<bool> m_capturing;
    std::atomic<bool> m_stopRequested;
    
    // Circular buffer for audio samples
    std::deque<AudioSample> m_buffer;
    QMutex m_bufferMutex;
    
    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int CHANNELS = 2;
    static constexpr int BUFFER_SECONDS = 300; // 5 minutes max
    
#ifdef _WIN32
    // Windows WASAPI
    IMMDeviceEnumerator* m_deviceEnumerator;
    IMMDevice* m_device;
    IAudioClient* m_audioClient;
    IAudioCaptureClient* m_captureClient;
    WAVEFORMATEX* m_waveFormat;
    bool m_comInitialized;
    
    bool initWASAPI();
    void cleanupWASAPI();
    void captureWASAPI();
#elif __APPLE__
    // macOS CoreAudio
    AudioQueueRef m_audioQueue;
    
    bool initCoreAudio();
    void cleanupCoreAudio();
    static void audioInputCallback(void* userData, AudioQueueRef queue,
                                   AudioQueueBufferRef buffer,
                                   const AudioTimeStamp* startTime,
                                   UInt32 numPackets,
                                   const AudioStreamPacketDescription* packetDesc);
#else
    // Linux PulseAudio
    pa_simple* m_pulseAudio;
    
    bool initPulseAudio();
    void cleanupPulseAudio();
    void capturePulseAudio();
#endif
};

#endif // AUDIOCAPTURE_H