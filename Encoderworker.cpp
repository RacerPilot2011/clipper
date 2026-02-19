/*
 * EncoderWorker.cpp
 *
 * Lightweight QObject wrapper designed to run `VideoEncoder::encode`
 * on a separate QThread. This class forwards encoder signals to the
 * UI and emits `finished()` when the worker function completes.
 */

#include "EncoderWorker.h"

EncoderWorker::EncoderWorker(VideoEncoder* encoder,
                             std::vector<VideoFrame> frames,
                             std::vector<AudioSample> mic,
                             std::vector<AudioSample> desktop,
                             VideoEncoder::EncodeOptions opts,
                             QObject* parent)
    : QObject(parent)
    , m_encoder(encoder)
    , m_frames(std::move(frames))
    , m_mic(std::move(mic))
    , m_desktop(std::move(desktop))
    , m_options(opts)
    , m_success(false)
{
    // Connect encoder signals to forward them
    connect(m_encoder, &VideoEncoder::progressUpdate,
            this, &EncoderWorker::progressUpdate);
    connect(m_encoder, &VideoEncoder::encodingComplete,
            this, &EncoderWorker::encodingComplete);
    connect(m_encoder, &VideoEncoder::errorOccurred,
            this, &EncoderWorker::errorOccurred);
}

void EncoderWorker::process() {
    m_success = m_encoder->encode(m_frames, m_mic, m_desktop, m_options);
    emit finished();
}