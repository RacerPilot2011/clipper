/*
 * ClipViewer.cpp
 *
 * Widget that provides simple playback controls for a saved clip. It uses
 * OpenCV's decoding APIs to read frames and Qt to render them. The code
 * keeps rendering on the GUI thread for simplicity.
 */

#include "ClipViewer.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QPixmap>

ClipViewer::ClipViewer(QWidget *parent)
    : QWidget(parent)
    , m_isPlaying(false)
    , m_totalFrames(0)
    , m_fps(30.0)
{
    setupUI();
}

ClipViewer::~ClipViewer() {
    releaseCurrentClip();
}

void ClipViewer::setupUI() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    
    // Video display
    m_videoLabel = new QLabel("No clip loaded");
    m_videoLabel->setAlignment(Qt::AlignCenter);
    m_videoLabel->setMinimumSize(640, 480);
    m_videoLabel->setStyleSheet("background-color: black; color: white;");
    mainLayout->addWidget(m_videoLabel);
    
    // Time display
    m_timeLabel = new QLabel("0:00 / 0:00");
    m_timeLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(m_timeLabel);
    
    // Controls
    QHBoxLayout* controlsLayout = new QHBoxLayout();
    
    m_playPauseBtn = new QPushButton("Play");
    m_playPauseBtn->setEnabled(false);
    connect(m_playPauseBtn, &QPushButton::clicked, this, &ClipViewer::onPlayPauseClicked);
    controlsLayout->addWidget(m_playPauseBtn);
    
    m_positionSlider = new QSlider(Qt::Horizontal);
    m_positionSlider->setEnabled(false);
    connect(m_positionSlider, &QSlider::sliderMoved, this, &ClipViewer::onSliderMoved);
    controlsLayout->addWidget(m_positionSlider);
    
    mainLayout->addLayout(controlsLayout);
    
    // Playback timer
    m_playbackTimer = new QTimer(this);
    connect(m_playbackTimer, &QTimer::timeout, this, &ClipViewer::updateFrame);
}

void ClipViewer::loadClip(const QString& filepath) {
    releaseCurrentClip();
    
    m_currentClipPath = filepath;
    m_capture.open(filepath.toStdString());
    
    if (m_capture.isOpened()) {
        m_totalFrames = static_cast<int>(m_capture.get(cv::CAP_PROP_FRAME_COUNT));
        m_fps = m_capture.get(cv::CAP_PROP_FPS);
        
        m_playPauseBtn->setEnabled(true);
        m_positionSlider->setEnabled(true);
        m_positionSlider->setMaximum(m_totalFrames - 1);
        
        showFrame(0);
        updateTimeDisplay();
    }
}

void ClipViewer::releaseCurrentClip() {
    if (m_isPlaying) {
        m_isPlaying = false;
        m_playbackTimer->stop();
        m_playPauseBtn->setText("Play");
    }
    
    if (m_capture.isOpened()) {
        m_capture.release();
    }
    
    m_videoLabel->clear();
    m_videoLabel->setText("No clip loaded");
    m_timeLabel->setText("0:00 / 0:00");
    m_positionSlider->setValue(0);
    m_playPauseBtn->setEnabled(false);
    m_positionSlider->setEnabled(false);
    
    m_currentClipPath.clear();
}

void ClipViewer::onPlayPauseClicked() {
    if (!m_capture.isOpened()) return;
    
    m_isPlaying = !m_isPlaying;
    
    if (m_isPlaying) {
        m_playPauseBtn->setText("Pause");
        int interval = static_cast<int>(1000.0 / m_fps);
        m_playbackTimer->start(interval);
    } else {
        m_playPauseBtn->setText("Play");
        m_playbackTimer->stop();
    }
}

void ClipViewer::onSliderMoved(int position) {
    showFrame(position);
    updateTimeDisplay();
}

void ClipViewer::updateFrame() {
    if (!m_capture.isOpened()) return;
    
    cv::Mat frame;
    if (m_capture.read(frame)) {
        displayFrame(frame);
        
        int currentPos = static_cast<int>(m_capture.get(cv::CAP_PROP_POS_FRAMES));
        m_positionSlider->setValue(currentPos);
        updateTimeDisplay();
    } else {
        // End of video
        m_isPlaying = false;
        m_playPauseBtn->setText("Play");
        m_playbackTimer->stop();
        m_capture.set(cv::CAP_PROP_POS_FRAMES, 0);
        m_positionSlider->setValue(0);
    }
}

void ClipViewer::showFrame(int frameNum) {
    if (!m_capture.isOpened()) return;
    
    m_capture.set(cv::CAP_PROP_POS_FRAMES, frameNum);
    cv::Mat frame;
    if (m_capture.read(frame)) {
        displayFrame(frame);
    }
}

void ClipViewer::displayFrame(const cv::Mat& frame) {
    cv::Mat rgb;
    cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);
    
    QImage img(rgb.data, rgb.cols, rgb.rows, rgb.step, QImage::Format_RGB888);
    QPixmap pixmap = QPixmap::fromImage(img);
    
    QPixmap scaled = pixmap.scaled(m_videoLabel->size(), 
                                   Qt::KeepAspectRatio, 
                                   Qt::SmoothTransformation);
    m_videoLabel->setPixmap(scaled);
}

void ClipViewer::updateTimeDisplay() {
    if (!m_capture.isOpened()) return;
    
    int currentFrame = static_cast<int>(m_capture.get(cv::CAP_PROP_POS_FRAMES));
    double currentTime = currentFrame / m_fps;
    double totalTime = m_totalFrames / m_fps;
    
    int currentMin = static_cast<int>(currentTime / 60);
    int currentSec = static_cast<int>(currentTime) % 60;
    int totalMin = static_cast<int>(totalTime / 60);
    int totalSec = static_cast<int>(totalTime) % 60;
    
    m_timeLabel->setText(QString("%1:%2 / %3:%4")
        .arg(currentMin)
        .arg(currentSec, 2, 10, QChar('0'))
        .arg(totalMin)
        .arg(totalSec, 2, 10, QChar('0')));
}