/*
 * AudioCapture.cpp
 *
 * Platform abstraction for capturing audio streams. The class supports
 * two logical device types: Microphone (capture input) and DesktopAudio
 * (system playback / loopback). The implementation delegates to a
 * platform-specific backend (WASAPI on Windows, AudioQueue on macOS,
 * PulseAudio on Linux).
 *
 * Important behavior:
 * - The object is a `QThread`. The `run()` method performs initialization
 *   and enters the capture loop. The capture loop appends `AudioSample`
 *   chunks to `m_buffer` while respecting a time-bound buffer limit.
 * - Public methods are safe to call from the GUI thread. Use signals to
 *   observe start/stop and error conditions.
 */

#include "AudioCapture.h"
#include <QDebug>
#include <QDateTime>
#include <cstring>

#ifdef _WIN32
#include <comdef.h>
#include <avrt.h>

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);
#endif

AudioCapture::AudioCapture(DeviceType type, QObject *parent)
    : QThread(parent)
    , m_deviceType(type)
    , m_capturing(false)
    , m_stopRequested(false)
#ifdef _WIN32
    , m_deviceEnumerator(nullptr)
    , m_device(nullptr)
    , m_audioClient(nullptr)
    , m_captureClient(nullptr)
    , m_waveFormat(nullptr)
    , m_comInitialized(false)
#elif __APPLE__
    , m_audioQueue(nullptr)
#else
    , m_pulseAudio(nullptr)
#endif
{
}

AudioCapture::~AudioCapture() {
    stopCapture();
    
#ifdef _WIN32
    cleanupWASAPI();
#elif __APPLE__
    cleanupCoreAudio();
#else
    cleanupPulseAudio();
#endif
}

QStringList AudioCapture::getAvailableDevices(DeviceType type) {
    QStringList devices;
    
#ifdef _WIN32
    // COM must be initialized on the calling thread before making any COM calls.
    // We use COINIT_APARTMENTTHREADED since this is an enumeration on a temporary context
    // that won't be accessed from other threads. Track whether we initialized COM so
    // we can balance the CoUninitialize call later.
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needsUninit = SUCCEEDED(hrInit);
    
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_MMDeviceEnumerator, nullptr,
        CLSCTX_ALL, IID_IMMDeviceEnumerator,
        (void**)&enumerator);
    
    if (SUCCEEDED(hr)) {
        IMMDeviceCollection* collection = nullptr;
        
        if (type == Microphone) {
            hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
        } else {
            hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
        }
        
        if (SUCCEEDED(hr)) {
            UINT count;
            collection->GetCount(&count);
            
            qDebug() << "Found" << count << (type == Microphone ? "microphone" : "desktop audio") << "devices";
            
            for (UINT i = 0; i < count; i++) {
                IMMDevice* device = nullptr;
                if (SUCCEEDED(collection->Item(i, &device))) {
                    LPWSTR deviceId = nullptr;
                    device->GetId(&deviceId);
                    
                    IPropertyStore* props = nullptr;
                    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
                        PROPVARIANT varName;
                        PropVariantInit(&varName);
                        
                        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName))) {
                            QString name = QString::fromWCharArray(varName.pwszVal);
                            QString id = QString::fromWCharArray(deviceId);
                            
                            if (type == DesktopAudio) {
                                devices.append(name + " (Loopback)|" + id);
                            } else {
                                devices.append(name + "|" + id);
                            }
                            
                            qDebug() << "  Device" << i << ":" << name;
                        }
                        
                        PropVariantClear(&varName);
                        props->Release();
                    }
                    
                    CoTaskMemFree(deviceId);
                    device->Release();
                }
            }
            collection->Release();
        }
        enumerator->Release();
    } else {
        qDebug() << "Failed to create device enumerator:" << QString::number(hr, 16);
    }
    
    if (needsUninit) {
        CoUninitialize();
    }
    
#elif __APPLE__
    // macOS CoreAudio enumeration
    AudioObjectPropertyAddress propertyAddress = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    
    UInt32 dataSize = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &propertyAddress, 0, nullptr, &dataSize);
    
    UInt32 deviceCount = dataSize / sizeof(AudioDeviceID);
    AudioDeviceID* audioDevices = new AudioDeviceID[deviceCount];
    
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, nullptr, &dataSize, audioDevices);
    
    for (UInt32 i = 0; i < deviceCount; i++) {
        AudioDeviceID deviceID = audioDevices[i];
        CFStringRef deviceName = nullptr;
        dataSize = sizeof(deviceName);
        propertyAddress.mSelector = kAudioDevicePropertyDeviceNameCFString;
        propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
        AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, nullptr, &dataSize, &deviceName);
        if (deviceName) {
            QString name = QString::fromCFString(deviceName);
            propertyAddress.mSelector = kAudioDevicePropertyStreamConfiguration;
            if (type == Microphone) {
                propertyAddress.mScope = kAudioDevicePropertyScopeInput;
            } else {
                propertyAddress.mScope = kAudioDevicePropertyScopeOutput;
            }
            AudioObjectGetPropertyDataSize(deviceID, &propertyAddress, 0, nullptr, &dataSize);
            if (dataSize > 0) {
                AudioBufferList* bufferList = (AudioBufferList*)malloc(dataSize);
                AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, nullptr, &dataSize, bufferList);
                if (bufferList->mNumberBuffers > 0) {
                    devices.append(name + "|" + QString::number(deviceID));
                }
                free(bufferList);
            }
            CFRelease(deviceName);
        }
    }
    delete[] audioDevices;
#else
    // Linux PulseAudio enumeration
    devices.append("Default|default");
#endif
    
    return devices;
}

bool AudioCapture::setDevice(const QString& deviceId) {
    if (m_capturing.load()) {
        emit errorOccurred("Cannot change device while capturing");
        return false;
    }
    m_deviceId = deviceId;
    qDebug() << "Audio device set to:" << deviceId;
    return true;
}

void AudioCapture::startCapture() {
    if (m_capturing.load()) {
        qDebug() << "Already capturing";
        return;
    }
    if (m_deviceId.isEmpty()) {
        emit errorOccurred("No device selected");
        return;
    }
    
    qDebug() << "Starting audio capture for device:" << m_deviceId;
    m_stopRequested = false;
    start();
}

void AudioCapture::stopCapture() {
    if (!isRunning()) {
        qDebug() << "Capture thread not running";
        return;
    }
    
    qDebug() << "Stopping audio capture...";
    m_stopRequested = true;

    // Wait for the capture thread to finish cleanly. We give it 30 seconds to
    // shut down gracefully. If it doesn't respond, force termination as a last resort
    // (though this can leave platform resources in an inconsistent state).
    if (!wait(30000)) {
        qDebug() << "Warning: Audio capture thread did not stop gracefully, forcing termination";
        terminate();
        wait();
    }
    qDebug() << "Audio capture stopped";
}

std::vector<AudioSample> AudioCapture::getBuffer(int seconds) {
    QMutexLocker locker(&m_bufferMutex);
    
    qDebug() << "Getting audio buffer for" << seconds << "seconds";
    qDebug() << "  Current buffer size:" << m_buffer.size() << "chunks";
    
    std::vector<AudioSample> result;
    if (m_buffer.empty()) {
        qDebug() << "  Buffer is empty!";
        return result;
    }

    // Iterate the buffer in reverse and collect chunks until we have at least
    // `seconds` duration of audio. We start from the back to get the most recent audio.
    double collected = 0;
    for (auto it = m_buffer.rbegin(); it != m_buffer.rend(); ++it) {
        if (it->channels > 0 && it->sampleRate > 0) {
            double chunkDuration = it->data.size() / (double)(it->channels * it->sampleRate);
            result.insert(result.begin(), *it);
            collected += chunkDuration;
            if (collected >= seconds) {
                break;
            }
        }
    }
    
    qDebug() << "  Retrieved" << result.size() << "chunks (" << collected << "seconds)";
    
    return result;
}

void AudioCapture::clearBuffer() {
    QMutexLocker locker(&m_bufferMutex);
    qDebug() << "Clearing audio buffer (" << m_buffer.size() << "chunks)";
    m_buffer.clear();
}

void AudioCapture::run() {
    qDebug() << "=== Audio capture thread started ===";
    qDebug() << "Device type:" << (m_deviceType == Microphone ? "Microphone" : "Desktop Audio");
    
#ifdef _WIN32
    // On Windows, each thread must initialize COM independently. We use MULTITHREADED
    // mode since this worker thread may be accessed by multiple components and we want
    // maximum compatibility with COM apartment threading models.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        qDebug() << "Failed to initialize COM library:" << QString::number(hr, 16);
        emit errorOccurred("Failed to initialize COM library");
        return;
    }
    m_comInitialized = true;
    qDebug() << "COM initialized successfully";

    if (!initWASAPI()) {
        qDebug() << "WASAPI initialization failed";
        cleanupWASAPI();
        CoUninitialize();
        m_comInitialized = false;
        return;
    }
    
    m_capturing = true;
    emit captureStarted();
    qDebug() << "Audio capture started successfully";
    
    captureWASAPI();
    
    qDebug() << "Audio capture finished, cleaning up...";
    cleanupWASAPI();
    CoUninitialize();
    m_comInitialized = false;
    
#elif __APPLE__
    if (!initCoreAudio()) {
        return;
    }
    m_capturing = true;
    emit captureStarted();
    while (!m_stopRequested.load()) {
        msleep(100);
    }
    cleanupCoreAudio();
#else
    if (!initPulseAudio()) {
        return;
    }
    m_capturing = true;
    emit captureStarted();
    capturePulseAudio();
    cleanupPulseAudio();
#endif
    
    m_capturing = false;
    emit captureStopped();
    qDebug() << "=== Audio capture thread stopped ===";
}

#ifdef _WIN32
bool AudioCapture::initWASAPI() {
    qDebug() << "[WASAPI Init] Starting initialization...";
    
    HRESULT hr;
    hr = CoCreateInstance(
        CLSID_MMDeviceEnumerator, nullptr,
        CLSCTX_ALL, IID_IMMDeviceEnumerator,
        (void**)&m_deviceEnumerator);
    
    if (FAILED(hr)) {
        qDebug() << "[WASAPI Init] Failed to create device enumerator:" << QString::number(hr, 16);
        emit errorOccurred("Failed to create device enumerator");
        return false;
    }
    qDebug() << "[WASAPI Init] Device enumerator created";
    
    QString deviceIdOnly = m_deviceId.split('|').last();
    qDebug() << "[WASAPI Init] Getting device:" << deviceIdOnly;
    
    std::wstring deviceIdW = deviceIdOnly.toStdWString();
    hr = m_deviceEnumerator->GetDevice(deviceIdW.c_str(), &m_device);
    if (FAILED(hr)) {
        qDebug() << "[WASAPI Init] Failed to get device:" << QString::number(hr, 16);
        emit errorOccurred("Failed to get device");
        return false;
    }
    qDebug() << "[WASAPI Init] Device obtained";
    
    hr = m_device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&m_audioClient);
    if (FAILED(hr)) {
        qDebug() << "[WASAPI Init] Failed to activate audio client:" << QString::number(hr, 16);
        emit errorOccurred("Failed to activate audio client");
        return false;
    }
    qDebug() << "[WASAPI Init] Audio client activated";

    hr = m_audioClient->GetMixFormat(&m_waveFormat);
    if (FAILED(hr)) {
        qDebug() << "[WASAPI Init] Failed to get mix format:" << QString::number(hr, 16);
        return false;
    }
    
    qDebug() << "[WASAPI Init] Audio format:";
    qDebug() << "  Sample rate:" << m_waveFormat->nSamplesPerSec << "Hz";
    qDebug() << "  Channels:" << m_waveFormat->nChannels;
    qDebug() << "  Bits per sample:" << m_waveFormat->wBitsPerSample;

    DWORD streamFlags = 0;
    if (m_deviceType == DesktopAudio) {
        streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        qDebug() << "[WASAPI Init] Using LOOPBACK mode for desktop audio";
    }

    // Set buffer duration to 100ms. REFERENCE_TIME is in 100-nanosecond units,
    // so 1000000 = 0.1 seconds. This balances latency (lower = more responsive)
    // with buffer stability and CPU efficiency.
    REFERENCE_TIME bufferDuration = 1000000;
    
    
    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED, 
        streamFlags, 
        bufferDuration, 
        0, 
        m_waveFormat, 
        nullptr);
        
    if (FAILED(hr)) {
        QString errorMsg = QString("Failed to initialize audio client: 0x%1").arg(hr, 0, 16);
        if (hr == AUDCLNT_E_ALREADY_INITIALIZED) {
            errorMsg += " (Already initialized)";
        } else if (hr == AUDCLNT_E_DEVICE_IN_USE) {
            errorMsg += " (Device in use)";
        } else if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
            errorMsg += " (Unsupported format)";
        }
        qDebug() << "[WASAPI Init]" << errorMsg;
        emit errorOccurred(errorMsg);
        return false;
    }
    qDebug() << "[WASAPI Init] Audio client initialized";

    hr = m_audioClient->GetService(IID_IAudioCaptureClient, (void**)&m_captureClient);
    if (FAILED(hr)) {
        qDebug() << "[WASAPI Init] Failed to get capture client:" << QString::number(hr, 16);
        return false;
    }
    qDebug() << "[WASAPI Init] Capture client obtained";

    hr = m_audioClient->Start();
    if (FAILED(hr)) {
        qDebug() << "[WASAPI Init] Failed to start audio client:" << QString::number(hr, 16);
        return false;
    }
    qDebug() << "[WASAPI Init] Audio client started successfully";

    return true;
}

void AudioCapture::cleanupWASAPI() {
    qDebug() << "[WASAPI Cleanup] Starting cleanup...";
    
    if (m_audioClient) {
        m_audioClient->Stop();
        qDebug() << "[WASAPI Cleanup] Audio client stopped";
    }
    
    if (m_captureClient) { 
        m_captureClient->Release(); 
        m_captureClient = nullptr; 
        qDebug() << "[WASAPI Cleanup] Capture client released";
    }
    
    if (m_audioClient) { 
        m_audioClient->Release(); 
        m_audioClient = nullptr; 
        qDebug() << "[WASAPI Cleanup] Audio client released";
    }
    
    if (m_waveFormat) { 
        CoTaskMemFree(m_waveFormat); 
        m_waveFormat = nullptr; 
        qDebug() << "[WASAPI Cleanup] Wave format freed";
    }
    
    if (m_device) { 
        m_device->Release(); 
        m_device = nullptr; 
        qDebug() << "[WASAPI Cleanup] Device released";
    }
    
    if (m_deviceEnumerator) { 
        m_deviceEnumerator->Release(); 
        m_deviceEnumerator = nullptr; 
        qDebug() << "[WASAPI Cleanup] Device enumerator released";
    }
    
    qDebug() << "[WASAPI Cleanup] Cleanup complete";
}

void AudioCapture::captureWASAPI() {
    qDebug() << "[WASAPI Capture] Starting capture loop...";
    
    // Request "Pro Audio" thread priority from Windows. This elevates our scheduling
    // priority to minimize missed capture windows and audio glitches.
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);
    if (hTask) {
        qDebug() << "[WASAPI Capture] Thread priority set to Pro Audio";
    }
    
    // Determine the audio format negotiated with the device. Some devices report
    // IEEE float (native), others use PCM integer which we must convert manually.
    bool isFloat = false;
    if (m_waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = true;
    } else if (m_waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE* waveFormatEx = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_waveFormat);
        isFloat = (waveFormatEx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    qDebug() << "[WASAPI Capture] Audio format:" << (isFloat ? "Float" : "PCM");
    
    int chunkCount = 0;
    size_t totalSamples = 0;

    while (!m_stopRequested.load()) {
        // Sleep briefly to allow other threads to run. 10ms is a reasonable balance
        // between CPU usage and capture responsiveness.
        Sleep(10);
        
        UINT32 packetLength = 0;
        HRESULT hr = m_captureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            qDebug() << "[WASAPI Capture] GetNextPacketSize failed:" << QString::number(hr, 16);
            break;
        }

        while (packetLength != 0) {
            BYTE* pData;
            UINT32 numFramesAvailable;
            DWORD flags;
            hr = m_captureClient->GetBuffer(&pData, &numFramesAvailable, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                qDebug() << "[WASAPI Capture] GetBuffer failed:" << QString::number(hr, 16);
                break;
            }

            if (numFramesAvailable == 0) {
                m_captureClient->ReleaseBuffer(numFramesAvailable);
                break;
            }

            AudioSample sample;
            sample.channels = m_waveFormat->nChannels;
            sample.sampleRate = m_waveFormat->nSamplesPerSec;
            sample.timestamp = QDateTime::currentMSecsSinceEpoch() / 1000.0;
            size_t totalSamplesInBuffer = numFramesAvailable * sample.channels;

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                sample.data.resize(totalSamplesInBuffer, 0.0f);
            } else {
                sample.data.reserve(totalSamplesInBuffer);
                if (isFloat) {
                    float* floatData = reinterpret_cast<float*>(pData);
                    sample.data.assign(floatData, floatData + totalSamplesInBuffer);
                } else {
                    // Convert 16-bit PCM to float in range [-1.0, 1.0]. Each sample is
                    // divided by 32768 (2^15) to normalize to float range. This preserves
                    // dynamic range while converting from integer to float representation.
                    int16_t* int16Data = reinterpret_cast<int16_t*>(pData);
                    sample.data.resize(totalSamplesInBuffer);
                    for (size_t i = 0; i < totalSamplesInBuffer; i++) {
                        sample.data[i] = int16Data[i] / 32768.0f;
                    }
                }
            }

            {
                QMutexLocker locker(&m_bufferMutex);
                m_buffer.push_back(sample);
                
                // Maintain the buffer with time-based pruning. We keep only the most recent
                // audio up to a maximum of 60 seconds. As new chunks arrive, old ones are
                // discarded once the total duration exceeds this limit. This prevents
                // unbounded memory growth while preserving instant replay data.
                double totalDuration = 0;
                for (const auto& s : m_buffer) {
                    if (s.channels > 0 && s.sampleRate > 0) {
                        totalDuration += s.data.size() / (double)(s.channels * s.sampleRate);
                    }
                }
                
                // Remove old chunks if we exceed the desired buffer time
                const double maxBufferSeconds = 60.0;
                while (totalDuration > maxBufferSeconds && m_buffer.size() > 1) {
                    const auto& oldSample = m_buffer.front();
                    if (oldSample.channels > 0 && oldSample.sampleRate > 0) {
                        totalDuration -= oldSample.data.size() / (double)(oldSample.channels * oldSample.sampleRate);
                    }
                    m_buffer.pop_front();
                }
            }
            
            chunkCount++;
            totalSamples += totalSamplesInBuffer;
            
            // Log first successful capture
            if (chunkCount == 1) {
                qDebug() << "[WASAPI Capture] First audio chunk captured!";
                qDebug() << "  Samples:" << totalSamplesInBuffer;
                qDebug() << "  Duration:" << (totalSamplesInBuffer / (double)(sample.channels * sample.sampleRate)) << "seconds";
            }
            
            // Periodic status
            if (chunkCount % 100 == 0) {
                double duration = totalSamples / (double)(m_waveFormat->nChannels * m_waveFormat->nSamplesPerSec);
                QMutexLocker locker(&m_bufferMutex);
                qDebug() << "[WASAPI Capture] Captured" << chunkCount << "chunks (" << duration << "seconds total," << m_buffer.size() << "in buffer)";
            }

            m_captureClient->ReleaseBuffer(numFramesAvailable);
            hr = m_captureClient->GetNextPacketSize(&packetLength);
        }
    }
    
    qDebug() << "[WASAPI Capture] Capture loop finished";
    qDebug() << "  Total chunks captured:" << chunkCount;
    qDebug() << "  Total samples:" << totalSamples;
    
    {
        QMutexLocker locker(&m_bufferMutex);
        qDebug() << "  Final buffer size:" << m_buffer.size() << "chunks";
    }
    
    if (hTask) {
        AvRevertMmThreadCharacteristics(hTask);
    }
}

#elif __APPLE__

void AudioCapture::audioInputCallback(void* userData, AudioQueueRef queue,
                                      AudioQueueBufferRef buffer,
                                      const AudioTimeStamp* startTime,
                                      UInt32 numPackets,
                                      const AudioStreamPacketDescription* packetDesc) {
    AudioCapture* capture = static_cast<AudioCapture*>(userData);
    
    if (!capture || capture->m_stopRequested.load()) {
        return;
    }
    
    AudioSample sample;
    sample.channels = CHANNELS;
    sample.sampleRate = SAMPLE_RATE;
    sample.timestamp = QDateTime::currentMSecsSinceEpoch() / 1000.0;
    
    // Copy audio data
    float* audioData = static_cast<float*>(buffer->mAudioData);
    size_t sampleCount = buffer->mAudioDataByteSize / sizeof(float);
    sample.data.assign(audioData, audioData + sampleCount);
    
    // Add to buffer
    {
        QMutexLocker locker(&capture->m_bufferMutex);
        capture->m_buffer.push_back(sample);
        
        // Prune old buffer chunks using the same time-based strategy as WASAPI.
        // This ensures consistent buffer behavior across platforms.
        double totalDuration = 0;
        for (const auto& s : capture->m_buffer) {
            if (s.channels > 0 && s.sampleRate > 0) {
                totalDuration += s.data.size() / (double)(s.channels * s.sampleRate);
            }
        }
        
        const double maxBufferSeconds = 60.0;
        while (totalDuration > maxBufferSeconds && capture->m_buffer.size() > 1) {
            const auto& oldSample = capture->m_buffer.front();
            if (oldSample.channels > 0 && oldSample.sampleRate > 0) {
                totalDuration -= oldSample.data.size() / (double)(oldSample.channels * oldSample.sampleRate);
            }
            capture->m_buffer.pop_front();
        }
    }
    
    // Re-enqueue buffer
    AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
}

bool AudioCapture::initCoreAudio() {
    AudioDeviceID deviceID = m_deviceId.split('|').last().toUInt();
    
    AudioStreamBasicDescription format;
    format.mSampleRate = SAMPLE_RATE;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = sizeof(float) * CHANNELS;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float) * CHANNELS;
    format.mChannelsPerFrame = CHANNELS;
    format.mBitsPerChannel = 32;
    
    OSStatus status = AudioQueueNewInput(
        &format,
        audioInputCallback,
        this,
        nullptr,
        kCFRunLoopCommonModes,
        0,
        &m_audioQueue);
    
    if (status != noErr) {
        emit errorOccurred(QString("Failed to create audio queue: %1").arg(status));
        return false;
    }
    
    // Set device
    UInt32 size = sizeof(deviceID);
    status = AudioQueueSetProperty(m_audioQueue, kAudioQueueProperty_CurrentDevice, &deviceID, size);
    
    if (status != noErr) {
        emit errorOccurred(QString("Failed to set audio device: %1").arg(status));
        AudioQueueDispose(m_audioQueue, true);
        m_audioQueue = nullptr;
        return false;
    }
    
    // Allocate buffers
    const int bufferSize = SAMPLE_RATE * CHANNELS * sizeof(float) / 2; // 0.5 second buffers
    for (int i = 0; i < 3; i++) {
        AudioQueueBufferRef buffer;
        status = AudioQueueAllocateBuffer(m_audioQueue, bufferSize, &buffer);
        if (status == noErr) {
            AudioQueueEnqueueBuffer(m_audioQueue, buffer, 0, nullptr);
        }
    }
    
    // Start queue
    status = AudioQueueStart(m_audioQueue, nullptr);
    if (status != noErr) {
        emit errorOccurred(QString("Failed to start audio queue: %1").arg(status));
        AudioQueueDispose(m_audioQueue, true);
        m_audioQueue = nullptr;
        return false;
    }
    
    return true;
}

void AudioCapture::cleanupCoreAudio() {
    if (m_audioQueue) {
        AudioQueueStop(m_audioQueue, true);
        AudioQueueDispose(m_audioQueue, true);
        m_audioQueue = nullptr;
    }
}

#else

bool AudioCapture::initPulseAudio() {
    pa_sample_spec spec;
    spec.format = PA_SAMPLE_FLOAT32LE;
    spec.channels = CHANNELS;
    spec.rate = SAMPLE_RATE;
    
    int error;
    const char* deviceName = m_deviceId.split('|').last().toUtf8().constData();
    
    m_pulseAudio = pa_simple_new(
        nullptr,
        "ScreenClipRecorder",
        m_deviceType == Microphone ? PA_STREAM_RECORD : PA_STREAM_RECORD,
        deviceName,
        "capture",
        &spec,
        nullptr,
        nullptr,
        &error);
    
    if (!m_pulseAudio) {
        emit errorOccurred(QString("PulseAudio error: %1").arg(pa_strerror(error)));
        return false;
    }
    
    return true;
}

void AudioCapture::cleanupPulseAudio() {
    if (m_pulseAudio) {
        pa_simple_free(m_pulseAudio);
        m_pulseAudio = nullptr;
    }
}

void AudioCapture::capturePulseAudio() {
    const int bufferSize = SAMPLE_RATE * CHANNELS / 2; // 0.5 second
    std::vector<float> buffer(bufferSize);
    
    while (!m_stopRequested.load()) {
        int error;
        if (pa_simple_read(m_pulseAudio, buffer.data(), bufferSize * sizeof(float), &error) < 0) {
            emit errorOccurred(QString("Read error: %1").arg(pa_strerror(error)));
            break;
        }
        
        AudioSample sample;
        sample.channels = CHANNELS;
        sample.sampleRate = SAMPLE_RATE;
        sample.timestamp = QDateTime::currentMSecsSinceEpoch() / 1000.0;
        sample.data = buffer;
        
        QMutexLocker locker(&m_bufferMutex);
        m_buffer.push_back(sample);
        
        // Apply the same time-based pruning logic as other platforms for consistency.
        double totalDuration = 0;
        for (const auto& s : m_buffer) {
            if (s.channels > 0 && s.sampleRate > 0) {
                totalDuration += s.data.size() / (double)(s.channels * s.sampleRate);
            }
        }
        
        const double maxBufferSeconds = 60.0;
        while (totalDuration > maxBufferSeconds && m_buffer.size() > 1) {
            const auto& oldSample = m_buffer.front();
            if (oldSample.channels > 0 && oldSample.sampleRate > 0) {
                totalDuration -= oldSample.data.size() / (double)(oldSample.channels * oldSample.sampleRate);
            }
            m_buffer.pop_front();
        }
    }
}

#endif