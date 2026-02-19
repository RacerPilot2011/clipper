#ifndef ENCODERWORKER_H
#define ENCODERWORKER_H

#include <QObject>
#include "VideoEncoder.h"
#include <vector>

class EncoderWorker : public QObject {
    Q_OBJECT
    
public:
    EncoderWorker(VideoEncoder* encoder,
                 std::vector<VideoFrame> frames,
                 std::vector<AudioSample> mic,
                 std::vector<AudioSample> desktop,
                 VideoEncoder::EncodeOptions opts,
                 QObject* parent = nullptr);
    
    bool success() const { return m_success; }
    
public slots:
    void process();
    
signals:
    void finished();
    void progressUpdate(int percent);
    void encodingComplete(bool success, const QString& message);
    void errorOccurred(const QString& error);
    
private:
    VideoEncoder* m_encoder;
    std::vector<VideoFrame> m_frames;
    std::vector<AudioSample> m_mic;
    std::vector<AudioSample> m_desktop;
    VideoEncoder::EncodeOptions m_options;
    bool m_success;
};

#endif // ENCODERWORKER_H