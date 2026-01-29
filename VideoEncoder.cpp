#include "VideoEncoder.h"
#include <QProcess>
#include <QTemporaryFile>
#include <QDebug>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <cmath>

#ifndef NOMINMAX
#define NOMINMAX
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

VideoEncoder::VideoEncoder(QObject *parent)
    : QObject(parent)
{
}

VideoEncoder::~VideoEncoder() {
}

std::vector<float> VideoEncoder::mixAudioSamples(
    const std::vector<AudioSample>& mic,
    const std::vector<AudioSample>& desktop)
{
    std::vector<float> mixed;
    
    if (mic.empty() && desktop.empty()) {
        return mixed;
    }
    
    // Concatenate all samples from each source
    std::vector<float> micData;
    for (const auto& sample : mic) {
        micData.insert(micData.end(), sample.data.begin(), sample.data.end());
    }
    
    std::vector<float> desktopData;
    for (const auto& sample : desktop) {
        desktopData.insert(desktopData.end(), sample.data.begin(), sample.data.end());
    }
    
    // If only one source, return it
    if (micData.empty()) return desktopData;
    if (desktopData.empty()) return micData;
    
    // Mix both sources
    size_t maxSize = (std::max)(micData.size(), desktopData.size());
    mixed.resize(maxSize, 0.0f);
    
    for (size_t i = 0; i < maxSize; i++) {
        float micSample = (i < micData.size()) ? micData[i] : 0.0f;
        float deskSample = (i < desktopData.size()) ? desktopData[i] : 0.0f;
        
        // Mix with slight attenuation to prevent clipping
        mixed[i] = (micSample * 0.7f + deskSample * 0.7f);
        
        // Soft clipping
        if (mixed[i] > 1.0f) mixed[i] = 1.0f;
        if (mixed[i] < -1.0f) mixed[i] = -1.0f;
    }
    
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
    
    // Mix audio
    std::vector<float> mixedAudio = mixAudioSamples(micAudio, desktopAudio);
    
    // Encode using FFmpeg libraries
    AVFormatContext* formatCtx = nullptr;
    AVCodecContext* videoCodecCtx = nullptr;
    AVCodecContext* audioCodecCtx = nullptr;
    AVStream* videoStream = nullptr;
    AVStream* audioStream = nullptr;
    SwsContext* swsCtx = nullptr;
    SwrContext* swrCtx = nullptr;
    
    bool success = false;
    
    try {
        // Allocate output format context
        avformat_alloc_output_context2(&formatCtx, nullptr, nullptr, options.outputPath.toUtf8().constData());
        if (!formatCtx) {
            throw std::runtime_error("Could not create output context");
        }
        
        // Get video dimensions from first frame
        int width = frames[0].image.width();
        int height = frames[0].image.height();
        
        // Find video encoder
        const AVCodec* videoCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!videoCodec) {
            throw std::runtime_error("H.264 encoder not found");
        }
        
        // Create video stream
        videoStream = avformat_new_stream(formatCtx, nullptr);
        if (!videoStream) {
            throw std::runtime_error("Failed to create video stream");
        }
        
        videoCodecCtx = avcodec_alloc_context3(videoCodec);
        if (!videoCodecCtx) {
            throw std::runtime_error("Failed to allocate video codec context");
        }
        
        // Configure video codec
        videoCodecCtx->codec_id = AV_CODEC_ID_H264;
        videoCodecCtx->codec_type = AVMEDIA_TYPE_VIDEO;
        videoCodecCtx->bit_rate = options.videoBitrate;
        videoCodecCtx->width = width;
        videoCodecCtx->height = height;
        videoCodecCtx->time_base = AVRational{1, options.fps};
        videoCodecCtx->framerate = AVRational{options.fps, 1};
        videoCodecCtx->gop_size = options.fps;
        videoCodecCtx->max_b_frames = 2;
        videoCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
        
        av_opt_set(videoCodecCtx->priv_data, "preset", "fast", 0);
        av_opt_set(videoCodecCtx->priv_data, "crf", "23", 0);
        
        if (formatCtx->oformat->flags & AVFMT_GLOBALHEADER) {
            videoCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        
        // Open video codec
        if (avcodec_open2(videoCodecCtx, videoCodec, nullptr) < 0) {
            throw std::runtime_error("Failed to open video codec");
        }
        
        avcodec_parameters_from_context(videoStream->codecpar, videoCodecCtx);
        videoStream->time_base = videoCodecCtx->time_base;
        
        // Setup audio if we have samples
        if (!mixedAudio.empty()) {
            const AVCodec* audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
            if (audioCodec) {
                audioStream = avformat_new_stream(formatCtx, nullptr);
                audioCodecCtx = avcodec_alloc_context3(audioCodec);
                
                audioCodecCtx->codec_id = AV_CODEC_ID_AAC;
                audioCodecCtx->codec_type = AVMEDIA_TYPE_AUDIO;
                audioCodecCtx->bit_rate = options.audioBitrate;
                audioCodecCtx->sample_rate = options.audioSampleRate;
                audioCodecCtx->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
                audioCodecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
                audioCodecCtx->time_base = AVRational{1, options.audioSampleRate};
                
                if (formatCtx->oformat->flags & AVFMT_GLOBALHEADER) {
                    audioCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                }
                
                if (avcodec_open2(audioCodecCtx, audioCodec, nullptr) >= 0) {
                    avcodec_parameters_from_context(audioStream->codecpar, audioCodecCtx);
                    audioStream->time_base = audioCodecCtx->time_base;
                }
            }
        }
        
        // Open output file
        if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&formatCtx->pb, options.outputPath.toUtf8().constData(), AVIO_FLAG_WRITE) < 0) {
                throw std::runtime_error("Could not open output file");
            }
        }
        
        // Write header
        if (avformat_write_header(formatCtx, nullptr) < 0) {
            throw std::runtime_error("Error writing header");
        }
        
        // Setup scaler for video
        swsCtx = sws_getContext(
            width, height, AV_PIX_FMT_RGB32,
            width, height, AV_PIX_FMT_YUV420P,
            SWS_BICUBIC, nullptr, nullptr, nullptr);
        
        // Allocate frame
        AVFrame* videoFrame = av_frame_alloc();
        videoFrame->format = AV_PIX_FMT_YUV420P;
        videoFrame->width = width;
        videoFrame->height = height;
        av_frame_get_buffer(videoFrame, 0);
        
        // Encode video frames
        int64_t pts = 0;
        for (size_t i = 0; i < frames.size(); i++) {
            static const QImage& img = frames[i].image;
            QImage rgbImg = img.convertToFormat(QImage::Format_RGB32);
            
            const uint8_t* srcData[1] = { rgbImg.constBits() };
            int srcLinesize[1] = { rgbImg.bytesPerLine() };
            
            sws_scale(swsCtx, srcData, srcLinesize, 0, height,
                     videoFrame->data, videoFrame->linesize);
            
            videoFrame->pts = pts++;
            
            // Send frame
            if (avcodec_send_frame(videoCodecCtx, videoFrame) >= 0) {
                AVPacket* pkt = av_packet_alloc();
                while (avcodec_receive_packet(videoCodecCtx, pkt) >= 0) {
                    av_packet_rescale_ts(pkt, videoCodecCtx->time_base, videoStream->time_base);
                    pkt->stream_index = videoStream->index;
                    av_interleaved_write_frame(formatCtx, pkt);
                }
                av_packet_free(&pkt);
            }
            
            // Update progress
            int progress = (i * 100) / frames.size();
            emit progressUpdate(progress);
        }
        
        // Flush video encoder
        avcodec_send_frame(videoCodecCtx, nullptr);
        AVPacket* pkt = av_packet_alloc();
        while (avcodec_receive_packet(videoCodecCtx, pkt) >= 0) {
            av_packet_rescale_ts(pkt, videoCodecCtx->time_base, videoStream->time_base);
            pkt->stream_index = videoStream->index;
            av_interleaved_write_frame(formatCtx, pkt);
        }
        av_packet_free(&pkt);
        
        av_frame_free(&videoFrame);
        
        // Encode audio if available
        if (audioCodecCtx && !mixedAudio.empty()) {
            // Setup resampler if needed
            swrCtx = swr_alloc();
            av_opt_set_chlayout(swrCtx, "in_chlayout", &audioCodecCtx->ch_layout, 0);
            av_opt_set_chlayout(swrCtx, "out_chlayout", &audioCodecCtx->ch_layout, 0);
            av_opt_set_int(swrCtx, "in_sample_rate", options.audioSampleRate, 0);
            av_opt_set_int(swrCtx, "out_sample_rate", options.audioSampleRate, 0);
            av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
            av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
            swr_init(swrCtx);
            
            // Encode audio frames
            AVFrame* audioFrame = av_frame_alloc();
            audioFrame->format = AV_SAMPLE_FMT_FLTP;
            audioFrame->ch_layout = audioCodecCtx->ch_layout;
            audioFrame->sample_rate = options.audioSampleRate;
            audioFrame->nb_samples = audioCodecCtx->frame_size;
            av_frame_get_buffer(audioFrame, 0);
            
            size_t audioIdx = 0;
            int64_t audioPts = 0;
            
            while (audioIdx < mixedAudio.size()) {
                size_t samplesNeeded = audioCodecCtx->frame_size * 2; // stereo
                size_t samplesAvailable = (std::min)(samplesNeeded, mixedAudio.size() - audioIdx);
                
                if (samplesAvailable > 0) {
                    // Deinterleave stereo to planar
                    float* leftChannel = (float*)audioFrame->data[0];
                    float* rightChannel = (float*)audioFrame->data[1];
                    
                    for (size_t i = 0; i < samplesAvailable / 2 && i < (size_t)audioCodecCtx->frame_size; i++) {
                        leftChannel[i] = mixedAudio[audioIdx + i * 2];
                        rightChannel[i] = mixedAudio[audioIdx + i * 2 + 1];
                    }
                    
                    audioFrame->pts = audioPts;
                    audioPts += audioCodecCtx->frame_size;
                    
                    if (avcodec_send_frame(audioCodecCtx, audioFrame) >= 0) {
                        AVPacket* audioPkt = av_packet_alloc();
                        while (avcodec_receive_packet(audioCodecCtx, audioPkt) >= 0) {
                            av_packet_rescale_ts(audioPkt, audioCodecCtx->time_base, audioStream->time_base);
                            audioPkt->stream_index = audioStream->index;
                            av_interleaved_write_frame(formatCtx, audioPkt);
                        }
                        av_packet_free(&audioPkt);
                    }
                }
                
                audioIdx += samplesAvailable;
            }
            
            // Flush audio encoder
            avcodec_send_frame(audioCodecCtx, nullptr);
            AVPacket* audioPkt = av_packet_alloc();
            while (avcodec_receive_packet(audioCodecCtx, audioPkt) >= 0) {
                av_packet_rescale_ts(audioPkt, audioCodecCtx->time_base, audioStream->time_base);
                audioPkt->stream_index = audioStream->index;
                av_interleaved_write_frame(formatCtx, audioPkt);
            }
            av_packet_free(&audioPkt);
            
            av_frame_free(&audioFrame);
        }
        
        // Write trailer
        av_write_trailer(formatCtx);
        
        success = true;
        emit encodingComplete(true, "Video encoded successfully");
        
    } catch (const std::exception& e) {
        emit errorOccurred(QString("Encoding error: %1").arg(e.what()));
        success = false;
    }
    
    // Cleanup
    if (swsCtx) sws_freeContext(swsCtx);
    if (swrCtx) swr_free(&swrCtx);
    if (videoCodecCtx) {
        avcodec_free_context(&videoCodecCtx);
    }
    if (audioCodecCtx) {
        avcodec_free_context(&audioCodecCtx);
    }
    if (formatCtx) {
        if (!(formatCtx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&formatCtx->pb);
        }
        avformat_free_context(formatCtx);
    }
    
    return success;
}