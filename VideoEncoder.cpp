/*
 * VideoEncoder.cpp
 *
 * Responsible for turning an ordered sequence of compressed video frames
 * (`VideoFrame`) and audio chunks (`AudioSample`) into a single output
 * container file (MP4). The implementation prefers to use an external
 * `ffmpeg` binary when available for robustness and performance. When
 * `ffmpeg` is not present, a best-effort OpenCV-based writer is used.
 *
 * Key responsibilities
 * - Validate frames and audio, write intermediate files if required by
 *   the chosen encoding path, and invoke the encoder process.
 * - Mix microphone and desktop audio streams with resampling and time
 *   alignment before writing to the container.
 * - Emit progress updates and detailed error messages for the UI.
 */

#include "VideoEncoder.h"
#include <QProcess>
#include <QTemporaryFile>
#include <QDebug>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QCoreApplication>
#include <QDataStream>
#include <cmath>
#include <opencv2/opencv.hpp>

VideoEncoder::VideoEncoder(QObject *parent)
    : QObject(parent)
{
}

VideoEncoder::~VideoEncoder() {
}

// Resample helper: convert an interleaved float buffer from srcRate to dstRate
std::vector<float> resampleAudio(const std::vector<float>& input, int srcRate, int dstRate, int channels) {
    if (srcRate == dstRate || input.empty()) return input;

    qDebug() << "Resampling audio from" << srcRate << "to" << dstRate << "Hz";
    
    double ratio = (double)srcRate / (double)dstRate;
    size_t inputFrames = input.size() / channels;
    size_t outFrames = (size_t)(inputFrames / ratio);
    
    std::vector<float> output;
    output.reserve(outFrames * channels);

    for (size_t i = 0; i < outFrames; i++) {
        // Linear interpolation between adjacent samples to approximate the value at
        // the target sample index. This is a simple but effective resampling method
        // that avoids expensive sinc interpolation while maintaining reasonable quality.
        double srcIndex = i * ratio;
        size_t index1 = (size_t)srcIndex;
        size_t index2 = index1 + 1;
        double frac = srcIndex - index1;

        for (int c = 0; c < channels; c++) {
            float val1 = 0.0f;
            float val2 = 0.0f;
            
            // Fetch the two surrounding samples, clamping bounds to avoid out-of-range access.
            size_t pos1 = index1 * channels + c;
            size_t pos2 = index2 * channels + c;
            
            if (pos1 < input.size()) val1 = input[pos1];
            if (pos2 < input.size()) val2 = input[pos2];
            
            // Linear blend: (1 - frac) * val1 + frac * val2
            output.push_back(val1 * (1.0 - frac) + val2 * frac);
        }
    }
    
    qDebug() << "Resampled from" << input.size() << "to" << output.size() << "samples";
    return output;
}

std::vector<float> monoToStereo(const std::vector<float>& mono) {
    std::vector<float> stereo;
    stereo.reserve(mono.size() * 2);
    // Duplicate each mono sample to both left and right channels. This is not ideal
    // for true stereo content but works for mono audio that needs to be presented as stereo.
    for (float sample : mono) {
        stereo.push_back(sample);
        stereo.push_back(sample);
    }
    return stereo;
}

std::vector<float> VideoEncoder::mixAudioSamples(
    const std::vector<AudioSample>& mic,
    const std::vector<AudioSample>& desktop)
{
    qDebug() << "=== Mixing audio samples ===";
    qDebug() << "Mic samples:" << mic.size() << "chunks";
    qDebug() << "Desktop samples:" << desktop.size() << "chunks";
    
    if (mic.empty() && desktop.empty()) {
        qDebug() << "No audio samples to mix";
        return {};
    }

    std::vector<float> micData;
    double micStartTime = 0;
    int micSampleRate = 48000;
    int micChannels = 2;
    
    if (!mic.empty()) {
        micStartTime = mic[0].timestamp;
        micSampleRate = mic[0].sampleRate;
        micChannels = mic[0].channels;
        
        for (const auto& s : mic) {
            micData.insert(micData.end(), s.data.begin(), s.data.end());
        }
        qDebug() << "Mic data size:" << micData.size() << "samples, rate:" << micSampleRate << "Hz, channels:" << micChannels;
    }

    std::vector<float> deskData;
    double deskStartTime = 0;
    int deskSampleRate = 48000;
    int deskChannels = 2;
    
    if (!desktop.empty()) {
        deskStartTime = desktop[0].timestamp;
        deskSampleRate = desktop[0].sampleRate;
        deskChannels = desktop[0].channels;
        
        for (const auto& s : desktop) {
            deskData.insert(deskData.end(), s.data.begin(), s.data.end());
        }
        qDebug() << "Desktop data size:" << deskData.size() << "samples, rate:" << deskSampleRate << "Hz, channels:" << deskChannels;
    }

    if (micData.empty()) {
        qDebug() << "Only desktop audio available";
        if (deskSampleRate != 48000 || deskChannels != 2) {
            if (deskChannels == 1) deskData = monoToStereo(deskData);
            if (deskSampleRate != 48000) deskData = resampleAudio(deskData, deskSampleRate, 48000, 2);
        }
        return deskData;
    }
    if (deskData.empty()) {
        qDebug() << "Only mic audio available";
        if (micSampleRate != 48000 || micChannels != 2) {
            if (micChannels == 1) micData = monoToStereo(micData);
            if (micSampleRate != 48000) micData = resampleAudio(micData, micSampleRate, 48000, 2);
        }
        return micData;
    }

    int targetRate = 48000;
    int targetChannels = 2;
    
    qDebug() << "Normalizing both streams to" << targetRate << "Hz stereo";
    
    if (micChannels == 1) {
        qDebug() << "Converting mic from mono to stereo";
        micData = monoToStereo(micData);
        micChannels = 2;
    }
    
    if (deskChannels == 1) {
        qDebug() << "Converting desktop from mono to stereo";
        deskData = monoToStereo(deskData);
        deskChannels = 2;
    }
    
    if (micSampleRate != targetRate) {
        qDebug() << "Resampling mic from" << micSampleRate << "Hz to" << targetRate << "Hz";
        micData = resampleAudio(micData, micSampleRate, targetRate, micChannels);
    }

    if (deskSampleRate != targetRate) {
        qDebug() << "Resampling desktop from" << deskSampleRate << "Hz to" << targetRate << "Hz";
        deskData = resampleAudio(deskData, deskSampleRate, targetRate, deskChannels);
    }

    qDebug() << "After normalization:";
    qDebug() << "  Mic:" << micData.size() << "samples";
    qDebug() << "  Desktop:" << deskData.size() << "samples";

    double timeDiff = deskStartTime - micStartTime;
    qDebug() << "Time difference:" << timeDiff << "seconds";
    
    size_t micSize = micData.size();
    size_t deskSize = deskData.size();
    
    size_t finalSize = std::max(micSize, deskSize);
    
    if (timeDiff > 0.1) {
        size_t offsetSamples = (size_t)(timeDiff * targetRate * targetChannels);
        finalSize = std::max(micSize, deskSize + offsetSamples);
    } else if (timeDiff < -0.1) {
        size_t offsetSamples = (size_t)(-timeDiff * targetRate * targetChannels);
        finalSize = std::max(micSize + offsetSamples, deskSize);
    }
    
    const size_t MAX_SAMPLES = 48000 * targetChannels * 3600;
    if (finalSize > MAX_SAMPLES) {
        qDebug() << "WARNING: Audio too long (" << finalSize/(targetRate*targetChannels) << "s), truncating to 1 hour";
        finalSize = MAX_SAMPLES;
    }

    qDebug() << "Final mixed audio size:" << finalSize << "samples (" << (finalSize/(targetRate*targetChannels)) << "seconds)";

    std::vector<float> mixed(finalSize, 0.0f);
    
    int micOffset = 0;
    int deskOffset = 0;
    
    if (timeDiff > 0.1) {
        deskOffset = (int)(timeDiff * targetRate * targetChannels);
    } else if (timeDiff < -0.1) {
        micOffset = (int)(-timeDiff * targetRate * targetChannels);
    }
    
    qDebug() << "Mixing with offsets - mic:" << micOffset << "desk:" << deskOffset;
    
    for (size_t i = 0; i < finalSize; i++) {
        float mVal = 0.0f;
        float dVal = 0.0f;

        long long micIdx = (long long)i - micOffset;
        if (micIdx >= 0 && micIdx < (long long)micSize) {
            mVal = micData[micIdx];
        }

        long long deskIdx = (long long)i - deskOffset;
        if (deskIdx >= 0 && deskIdx < (long long)deskSize) {
            dVal = deskData[deskIdx];
        }

        float sum = mVal + dVal;
        
        if (sum > 1.0f) {
            sum = 1.0f - (1.0f / (1.0f + (sum - 1.0f)));
        } else if (sum < -1.0f) {
            sum = -1.0f + (1.0f / (1.0f + (-sum - 1.0f)));
        }
        
        mixed[i] = sum * 0.95f;
    }

    qDebug() << "Audio mixing complete!";
    return mixed;
}

bool VideoEncoder::encode(
    const std::vector<VideoFrame>& frames,
    const std::vector<AudioSample>& micAudio,
    const std::vector<AudioSample>& desktopAudio,
    const EncodeOptions& options)
{
    if (frames.empty()) {
        emit errorOccurred("No frames to encode");
        return false;
    }
    
    try {
        if (frames[0].jpegData.isEmpty()) {
            emit errorOccurred("No frame data available");
            return false;
        }

        QString ffmpegPath = findFFmpegPath();
        if (!ffmpegPath.isEmpty()) {
            qDebug() << "Using FFmpeg for encoding";
            return encodeWithFFmpeg(frames, micAudio, desktopAudio, options, ffmpegPath);
        }
        
        qDebug() << "FFmpeg not found, using OpenCV fallback";
        return encodeWithOpenCV(frames, micAudio, desktopAudio, options);
        
    } catch (const std::exception& e) {
        emit errorOccurred(QString("Encoding error: %1").arg(e.what()));
        return false;
    }
}

bool VideoEncoder::encodeWithFFmpeg(
    const std::vector<VideoFrame>& frames,
    const std::vector<AudioSample>& micAudio,
    const std::vector<AudioSample>& desktopAudio,
    const EncodeOptions& options,
    const QString& ffmpegPath)
{
    qDebug() << "=== Starting FFmpeg encoding ===";

    if (frames.empty()) {
        emit errorOccurred("No frames provided");
        return false;
    }

    QImage firstFrame;
    if (!firstFrame.loadFromData(frames[0].jpegData, "JPEG")) {
        emit errorOccurred("Failed to decode first frame");
        return false;
    }

    const int width  = firstFrame.width();
    const int height = firstFrame.height();

    if (width <= 0 || height <= 0 || width > 7680 || height > 4320) {
        emit errorOccurred(QString("Invalid video dimensions: %1x%2").arg(width).arg(height));
        return false;
    }

    const QString tempDir =
        QDir::toNativeSeparators(
            QDir::tempPath() + "/screenclip_" +
            QString::number(QDateTime::currentSecsSinceEpoch())
        );

    const QString framesDir = tempDir + "/frames";
    QDir().mkpath(framesDir);

    const QString frameListPath = tempDir + "/frames.txt";
    const QString audioPath     = tempDir + "/audio.wav";
    const QString ffmpegLogPath = tempDir + "/ffmpeg.log";

    QFile frameListFile(frameListPath);
    if (!frameListFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        emit errorOccurred("Failed to create frame list");
        return false;
    }

    QTextStream frameList(&frameListFile);

    int validFrames = 0;
    QString lastFramePath;

    for (size_t i = 0; i < frames.size(); ++i) {
        const QByteArray& data = frames[i].jpegData;

        if (data.size() < 2 || (uchar)data[0] != 0xFF || (uchar)data[1] != 0xD8)
            continue;

        QImage img;
        if (!img.loadFromData(data, "JPEG") ||
            img.width() != width ||
            img.height() != height)
            continue;

        const QString framePath =
            framesDir + QString("/frame_%1.jpg").arg(i, 6, 10, QChar('0'));

        QFile f(framePath);
        if (!f.open(QIODevice::WriteOnly))
            continue;

        f.write(data);
        f.close();

        frameList << "file '" << framePath << "'\n";
        lastFramePath = framePath;
        validFrames++;
    }

    if (validFrames == 0) {
        emit errorOccurred("No valid frames written");
        return false;
    }

    frameList << "file '" << lastFramePath << "'\n";
    frameListFile.close();

    bool hasAudio = false;
    try {
        auto mixed = mixAudioSamples(micAudio, desktopAudio);
        if (!mixed.empty() &&
            saveAudioToWav(mixed, audioPath, 48000) &&
            QFileInfo(audioPath).size() > 0)
        {
            hasAudio = true;
        }
    } catch (...) {}

    QStringList args;
    args << "-y"
         << "-loglevel" << "verbose"
         << "-logfile" << ffmpegLogPath
         << "-framerate" << QString::number(options.fps)
         << "-f" << "concat"
         << "-safe" << "0"
         << "-i" << frameListPath;

    if (hasAudio)
        args << "-i" << audioPath;

    args << "-c:v" << "libx264"
         << "-preset" << "medium"
         << "-crf" << "23"
         << "-pix_fmt" << "yuv420p";

    if (hasAudio) {
        args << "-c:a" << "aac"
             << "-b:a" << "192k"
             << "-ar" << "48000"
             << "-ac" << "2"
             << "-shortest";
    } else {
        args << "-an";
    }

    args << "-movflags" << "+faststart"
         << options.outputPath;

    QProcess ffmpeg;
    ffmpeg.setWorkingDirectory(tempDir);
    ffmpeg.start(ffmpegPath, args);

    if (!ffmpeg.waitForStarted(5000)) {
        emit errorOccurred("Failed to start FFmpeg");
        return false;
    }

    ffmpeg.waitForFinished(-1);

    if (ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0) {
        QString err = "FFmpeg failed.\n\nLog file:\n" + ffmpegLogPath;
        emit errorOccurred(err);
        return false;
    }

    if (!QFile::exists(options.outputPath) ||
        QFileInfo(options.outputPath).size() == 0)
    {
        emit errorOccurred("Output file not created.\nLog:\n" + ffmpegLogPath);
        return false;
    }

    QDir(tempDir).removeRecursively();
    emit encodingComplete(true, "Video saved successfully");
    return true;
}

bool VideoEncoder::encodeWithOpenCV(
    const std::vector<VideoFrame>& frames, 
    const std::vector<AudioSample>& micAudio,
    const std::vector<AudioSample>& desktopAudio,
    const EncodeOptions& options)
{
    qDebug() << "=== Using OpenCV fallback encoder ===";
    qDebug() << "WARNING: Audio will not be included";
    
    try {
        QImage firstFrame;
        if (!firstFrame.loadFromData(frames[0].jpegData, "JPEG")) {
            emit errorOccurred("Failed to decompress first frame");
            return false;
        }
        
        int width = firstFrame.width();
        int height = firstFrame.height();
        
        qDebug() << "Video dimensions:" << width << "x" << height;
        
        cv::VideoWriter writer;
        writer.open(options.outputPath.toStdString(),
                   cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
                   static_cast<double>(options.fps),
                   cv::Size(width, height));
        
        if (!writer.isOpened()) {
            emit errorOccurred("Failed to create video file");
            return false;
        }
        
        qDebug() << "Writing" << frames.size() << "frames...";
        
        for (size_t i = 0; i < frames.size(); i++) {
            QImage img;
            if (!img.loadFromData(frames[i].jpegData, "JPEG")) {
                continue;
            }
            
            img = img.convertToFormat(QImage::Format_RGB888);
            cv::Mat mat(img.height(), img.width(), CV_8UC3, 
                       const_cast<uchar*>(img.bits()), 
                       img.bytesPerLine());
            
            cv::Mat bgr;
            cv::cvtColor(mat, bgr, cv::COLOR_RGB2BGR);
            writer.write(bgr);
            
            if (i % 30 == 0) {
                emit progressUpdate(static_cast<int>((i * 100) / frames.size()));
            }
        }
        
        writer.release();
        
        if (!QFile::exists(options.outputPath)) {
            emit errorOccurred("Video file was not created");
            return false;
        }
        
        emit progressUpdate(100);
        emit encodingComplete(true, "Video saved (OpenCV mode - no audio)");
        return true;
        
    } catch (const std::exception& e) {
        emit errorOccurred(QString("OpenCV error: %1").arg(e.what()));
        return false;
    }
}

bool VideoEncoder::saveAudioToWav(const std::vector<float>& samples, const QString& filepath, int sampleRate)
{
    if (samples.empty()) {
        return false;
    }
    
    qDebug() << "Saving WAV:" << filepath;
    qDebug() << "  Samples:" << samples.size();
    qDebug() << "  Duration:" << (samples.size() / 2.0 / sampleRate) << "s";
    
    QFile file(filepath);
    if (!file.open(QIODevice::WriteOnly)) {
        qDebug() << "Failed to open WAV file:" << file.errorString();
        return false;
    }
    
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    
    size_t numSamples = samples.size();
    if (numSamples % 2 != 0) {
        numSamples--;
    }
    
    int channels = 2;
    size_t dataSize = numSamples * sizeof(float);
    quint32 fileSize = 36 + static_cast<quint32>(dataSize);
    
    out.writeRawData("RIFF", 4);
    out << fileSize;
    out.writeRawData("WAVE", 4);
    out.writeRawData("fmt ", 4);
    
    quint32 fmtChunkSize = 16;
    quint16 audioFormat = 3; // IEEE float
    quint16 numChannels = static_cast<quint16>(channels);
    quint32 sampRate = static_cast<quint32>(sampleRate);
    quint32 byteRate = sampRate * channels * sizeof(float);
    quint16 blockAlign = static_cast<quint16>(channels * sizeof(float));
    quint16 bitsPerSample = 32;
    
    out << fmtChunkSize << audioFormat << numChannels << sampRate << byteRate << blockAlign << bitsPerSample;
    
    out.writeRawData("data", 4);
    out << static_cast<quint32>(dataSize);
    
    for (size_t i = 0; i < numSamples; i++) {
        out << samples[i];
    }
    
    file.close();
    
    qDebug() << "WAV saved:" << QFileInfo(filepath).size() << "bytes";
    return file.error() == QFile::NoError;
}

QString VideoEncoder::findFFmpegPath()
{
    qDebug() << "Searching for FFmpeg...";
    
    QStringList possiblePaths;
    
#ifdef Q_OS_WINDOWS
    QString appDir = QCoreApplication::applicationDirPath();
    possiblePaths << appDir + "/ffmpeg.exe"
                  << appDir + "/ffmpeg/bin/ffmpeg.exe"
                  << appDir + "/../ffmpeg.exe"
                  << "C:/ffmpeg/bin/ffmpeg.exe"
                  << "ffmpeg.exe";
#elif defined(Q_OS_MACOS)
    possiblePaths << "/usr/local/bin/ffmpeg"
                  << "/opt/homebrew/bin/ffmpeg"
                  << "/opt/local/bin/ffmpeg"
                  << QCoreApplication::applicationDirPath() + "/ffmpeg"
                  << "ffmpeg";
#else
    possiblePaths << "/usr/bin/ffmpeg"
                  << "/usr/local/bin/ffmpeg"
                  << QCoreApplication::applicationDirPath() + "/ffmpeg"
                  << "ffmpeg";
#endif
    
    for (const QString& path : possiblePaths) {
        if (QFile::exists(path)) {
            qDebug() << "Found FFmpeg at:" << path;
            return path;
        }
        
        QProcess process;
        process.start(path, QStringList() << "-version");
        if (process.waitForFinished(1000)) {
            if (process.exitCode() == 0) {
                qDebug() << "Found FFmpeg in PATH:" << path;
                return path;
            }
        }
    }
    
    qDebug() << "FFmpeg not found";
    return QString();
}