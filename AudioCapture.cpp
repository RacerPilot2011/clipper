#include "AudioCapture.h"
#include <QDebug>
#include <QDateTime>
#include <cstring>

#ifndef NOMINMAX
#define NOMINMAX
#endif

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
#elif __APPLE__
    , m_audioQueue(nullptr)
#else
    , m_pulseAudio(nullptr)
#endif
{
#ifdef _WIN32
    CoInitialize(nullptr);
#endif
}

AudioCapture::~AudioCapture() {
    stopCapture();
    
#ifdef _WIN32
    cleanupWASAPI();
    CoUninitialize();
#elif __APPLE__
    cleanupCoreAudio();
#else
    cleanupPulseAudio();
#endif
}

QStringList AudioCapture::getAvailableDevices(DeviceType type) {
    QStringList devices;
    
#ifdef _WIN32
    // Windows WASAPI enumeration
    CoInitialize(nullptr);
    
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
                                devices.append(name + " (WASAPI Loopback)|" + id);
                            } else {
                                devices.append(name + "|" + id);
                            }
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
    }
    
    CoUninitialize();
    
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
        
        // Get device name
        CFStringRef deviceName = nullptr;
        dataSize = sizeof(deviceName);
        propertyAddress.mSelector = kAudioDevicePropertyDeviceNameCFString;
        propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
        
        AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, nullptr, &dataSize, &deviceName);
        
        if (deviceName) {
            QString name = QString::fromCFString(deviceName);
            
            // Check if device has input or output capabilities
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
    // Note: This is simplified - full implementation would use pa_context
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
    return true;
}

void AudioCapture::startCapture() {
    if (m_capturing.load()) {
        return;
    }
    
    if (m_deviceId.isEmpty()) {
        emit errorOccurred("No device selected");
        return;
    }
    
    m_stopRequested = false;
    start();
}

void AudioCapture::stopCapture() {
    m_stopRequested = true;
    wait(5000);
}

std::vector<AudioSample> AudioCapture::getBuffer(int seconds) {
    QMutexLocker locker(&m_bufferMutex);
    
    int samplesToGet = seconds * 2; // Assuming 0.5s chunks
    std::vector<AudioSample> result;
    
    int startIdx = (std::max)(0, (int)m_buffer.size() - samplesToGet);
    for (size_t i = startIdx; i < m_buffer.size(); i++) {
        result.push_back(m_buffer[i]);
    }
    
    return result;
}

void AudioCapture::clearBuffer() {
    QMutexLocker locker(&m_bufferMutex);
    m_buffer.clear();
}

void AudioCapture::run() {
#ifdef _WIN32
    if (!initWASAPI()) {
        return;
    }
    
    m_capturing = true;
    emit captureStarted();
    
    captureWASAPI();
    
    cleanupWASAPI();
    
#elif __APPLE__
    if (!initCoreAudio()) {
        return;
    }
    
    m_capturing = true;
    emit captureStarted();
    
    // CoreAudio uses callbacks, so just wait
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
}

#ifdef _WIN32

bool AudioCapture::initWASAPI() {
    HRESULT hr;
    
    // Create device enumerator
    hr = CoCreateInstance(
        CLSID_MMDeviceEnumerator, nullptr,
        CLSCTX_ALL, IID_IMMDeviceEnumerator,
        (void**)&m_deviceEnumerator);
    
    if (FAILED(hr)) {
        emit errorOccurred("Failed to create device enumerator");
        return false;
    }
    
    // Get device
    LPWSTR deviceIdW = (LPWSTR)m_deviceId.split('|').last().toStdWString().c_str();
    hr = m_deviceEnumerator->GetDevice(deviceIdW, &m_device);
    
    if (FAILED(hr)) {
        emit errorOccurred("Failed to get device");
        return false;
    }
    
    // Activate audio client
    hr = m_device->Activate(
        IID_IAudioClient, CLSCTX_ALL,
        nullptr, (void**)&m_audioClient);
    
    if (FAILED(hr)) {
        emit errorOccurred("Failed to activate audio client");
        return false;
    }
    
    // Get mix format
    hr = m_audioClient->GetMixFormat(&m_waveFormat);
    if (FAILED(hr)) {
        emit errorOccurred("Failed to get mix format");
        return false;
    }
    
    // Initialize audio client
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    
    // For desktop audio (render devices), add loopback flag
    if (m_deviceType == DesktopAudio) {
        streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }
    
    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        streamFlags,
        10000000, // 1 second buffer
        0,
        m_waveFormat,
        nullptr);
    
    if (FAILED(hr)) {
        emit errorOccurred(QString("Failed to initialize audio client: 0x%1").arg(hr, 0, 16));
        return false;
    }
    
    // Get capture client
    hr = m_audioClient->GetService(IID_IAudioCaptureClient, (void**)&m_captureClient);
    if (FAILED(hr)) {
        emit errorOccurred("Failed to get capture client");
        return false;
    }
    
    // Start audio client
    hr = m_audioClient->Start();
    if (FAILED(hr)) {
        emit errorOccurred("Failed to start audio client");
        return false;
    }
    
    return true;
}

void AudioCapture::cleanupWASAPI() {
    if (m_audioClient) {
        m_audioClient->Stop();
    }
    
    if (m_captureClient) {
        m_captureClient->Release();
        m_captureClient = nullptr;
    }
    
    if (m_audioClient) {
        m_audioClient->Release();
        m_audioClient = nullptr;
    }
    
    if (m_waveFormat) {
        CoTaskMemFree(m_waveFormat);
        m_waveFormat = nullptr;
    }
    
    if (m_device) {
        m_device->Release();
        m_device = nullptr;
    }
    
    if (m_deviceEnumerator) {
        m_deviceEnumerator->Release();
        m_deviceEnumerator = nullptr;
    }
}

void AudioCapture::captureWASAPI() {
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);
    
    while (!m_stopRequested.load()) {
        Sleep(10);
        
        UINT32 packetLength = 0;
        HRESULT hr = m_captureClient->GetNextPacketSize(&packetLength);
        
        if (FAILED(hr)) {
            break;
        }
        
        while (packetLength != 0) {
            BYTE* pData;
            UINT32 numFramesAvailable;
            DWORD flags;
            
            hr = m_captureClient->GetBuffer(
                &pData,
                &numFramesAvailable,
                &flags,
                nullptr,
                nullptr);
            
            if (FAILED(hr)) {
                break;
            }
            
            // Convert audio data
            AudioSample sample;
            sample.channels = m_waveFormat->nChannels;
            sample.sampleRate = m_waveFormat->nSamplesPerSec;
            sample.timestamp = QDateTime::currentMSecsSinceEpoch() / 1000.0;
            
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                sample.data.resize(numFramesAvailable * sample.channels, 0.0f);
            } else {
                // Convert to float samples
                if (m_waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                    (m_waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                     reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_waveFormat)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                    
                    float* floatData = reinterpret_cast<float*>(pData);
                    sample.data.assign(floatData, floatData + (numFramesAvailable * sample.channels));
                } else {
                    // Convert from int16 to float
                    int16_t* int16Data = reinterpret_cast<int16_t*>(pData);
                    sample.data.resize(numFramesAvailable * sample.channels);
                    
                    for (UINT32 i = 0; i < numFramesAvailable * sample.channels; i++) {
                        sample.data[i] = int16Data[i] / 32768.0f;
                    }
                }
            }
            
            // Add to buffer
            {
                QMutexLocker locker(&m_bufferMutex);
                m_buffer.push_back(sample);
                
                // Limit buffer size
                while (m_buffer.size() > BUFFER_SECONDS * 2) {
                    m_buffer.pop_front();
                }
            }
            
            m_captureClient->ReleaseBuffer(numFramesAvailable);
            
            hr = m_captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                break;
            }
        }
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
        
        while (capture->m_buffer.size() > BUFFER_SECONDS * 2) {
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
        
        while (m_buffer.size() > BUFFER_SECONDS * 2) {
            m_buffer.pop_front();
        }
    }
}

#endif