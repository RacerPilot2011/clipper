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

    // Encode video with audio
    bool encode(const std::vector<VideoFrame>& frames,
                const std::vector<AudioSample>& micAudio,
                const std::vector<AudioSample>& desktopAudio,
                const EncodeOptions& options);

signals:
    void progressUpdate(int percent);
    void encodingComplete(bool success, const QString& message);
    void errorOccurred(const QString& error);

private:
    std::vector<float> mixAudioSamples(const std::vector<AudioSample>& mic,
                                       const std::vector<AudioSample>& desktop);
    
    bool encodeWithFFmpeg(const std::vector<VideoFrame>& frames,
                         const std::vector<float>& audioSamples,
                         const EncodeOptions& options);
    
    bool checkFFmpegAvailable();
    QString findFFmpegPath();
};

#endif // VIDEOENCODER_H