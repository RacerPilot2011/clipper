#ifndef VIDEOENCODER_H
#define VIDEOENCODER_H

#include <QObject>
#include <QString>
#include <QImage>
#include <vector>
#include "AudioCapture.h"
#include "ScreenRecorder.h"

class VideoEncoder : public QObject {
    Q_OBJECT

public:
    explicit VideoEncoder(QObject *parent = nullptr);
    ~VideoEncoder();

    struct EncodeOptions {
        QString outputPath;
        int fps = 30;
        int videoBitrate = 5000000; // 5 Mbps
        int audioBitrate = 192000;  // 192 kbps
        int audioSampleRate = 48000;
    };

    /*
     * Encode video with audio
     * - `encode` coordinates turning an in-memory frame/audio buffer into
     *   a single MP4 (H.264 + AAC) file as specified by `options`.
     * - Implementation strategy: prefer an external `ffmpeg` binary when
     *   available (reliable and feature-complete). Fall back to an OpenCV
     *   based writer when `ffmpeg` cannot be found.
     * - The method emits `progressUpdate` to report percent-complete and
     *   `encodingComplete` upon finish. Any recoverable or fatal error is
     *   reported via `errorOccurred`.
     */
    bool encode(const std::vector<VideoFrame>& frames,
                const std::vector<AudioSample>& micAudio,
                const std::vector<AudioSample>& desktopAudio,
                const EncodeOptions& options);

signals:
    void progressUpdate(int percent);
    void encodingComplete(bool success, const QString& message);
    void errorOccurred(const QString& error);

private:
    /*
     * mixAudioSamples
     * - Merge two streams (mic and desktop) into a single interleaved
     *   float buffer at the target sample rate and channel count.
     * - Handles channel expansion (mono->stereo), resampling, time
     *   alignment using timestamps, and soft clipping to avoid overflow.
     */
    std::vector<float> mixAudioSamples(const std::vector<AudioSample>& mic,
                                       const std::vector<AudioSample>& desktop);
    
    bool saveAudioToWav(const std::vector<float>& samples, const QString& filepath, int sampleRate);
    QString findFFmpegPath();
    
    bool encodeWithFFmpeg(const std::vector<VideoFrame>& frames,
                         const std::vector<AudioSample>& micAudio,
                         const std::vector<AudioSample>& desktopAudio,
                         const EncodeOptions& options,
                         const QString& ffmpegPath);
    
    bool encodeWithOpenCV(const std::vector<VideoFrame>& frames,
                         const std::vector<AudioSample>& micAudio,
                         const std::vector<AudioSample>& desktopAudio,
                         const EncodeOptions& options);
};

#endif // VIDEOENCODER_H