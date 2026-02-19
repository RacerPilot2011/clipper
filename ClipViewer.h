#ifndef CLIPVIEWER_H
#define CLIPVIEWER_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <opencv2/opencv.hpp>

/*
 * ClipViewer
 *
 * Purpose
 * - A lightweight widget that provides basic playback controls for a saved
 *   clip. It uses OpenCV's `VideoCapture` to decode frames and displays them
 *   via Qt widgets. This component is intentionally simple: it is a viewer
 *   (not an editor) and is optimized for responsiveness rather than feature
 *   completeness.
 *
 * Responsibilities
 * - Load a file path and probe the video for frame count and FPS.
 * - Provide play/pause, position seeking and a textual time display.
 * - Convert OpenCV BGR frames into Qt-friendly RGB images and scale them
 *   for display while keeping aspect ratio.
 *
 * Threading and performance
 * - All operations run on the GUI thread. Decoding is performed using
 *   OpenCV's synchronous API; for very large files or slow codecs consider
 *   moving decoding to a worker thread.
 */

class ClipViewer : public QWidget {
    Q_OBJECT

public:
    explicit ClipViewer(QWidget *parent = nullptr);
    ~ClipViewer();

    void loadClip(const QString& filepath);
    void releaseCurrentClip();
    QString currentClipPath() const { return m_currentClipPath; }

private slots:
    void onPlayPauseClicked();
    void onSliderMoved(int position);
    void updateFrame();

private:
    void setupUI();
    void showFrame(int frameNum);
    void displayFrame(const cv::Mat& frame);
    void updateTimeDisplay();

    QLabel* m_videoLabel;
    QLabel* m_timeLabel;
    QPushButton* m_playPauseBtn;
    QSlider* m_positionSlider;
    
    QTimer* m_playbackTimer;
    cv::VideoCapture m_capture;
    
    QString m_currentClipPath;
    bool m_isPlaying;
    int m_totalFrames;
    double m_fps;
};

#endif // CLIPVIEWER_H