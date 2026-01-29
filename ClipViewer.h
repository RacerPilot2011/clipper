#ifndef CLIPVIEWER_H
#define CLIPVIEWER_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <opencv2/opencv.hpp>

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