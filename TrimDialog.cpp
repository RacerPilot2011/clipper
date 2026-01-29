#include "TrimDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QPixmap>

TrimDialog::TrimDialog(const QString& videoPath, QWidget *parent)
    : QDialog(parent)
    , m_videoPath(videoPath)
    , m_startFrame(0)
    , m_endFrame(0)
    , m_totalFrames(0)
    , m_fps(30.0)
{
    m_capture.open(videoPath.toStdString());
    
    if (m_capture.isOpened()) {
        m_totalFrames = static_cast<int>(m_capture.get(cv::CAP_PROP_FRAME_COUNT));
        m_fps = m_capture.get(cv::CAP_PROP_FPS);
        m_endFrame = m_totalFrames - 1;
    }
    
    setupUI();
    showFrame(0);
}

TrimDialog::~TrimDialog() {
    if (m_capture.isOpened()) {
        m_capture.release();
    }
}

void TrimDialog::setupUI() {
    setWindowTitle("Trim Clip");
    setMinimumSize(800, 600);
    
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    
    // Preview
    m_previewLabel = new QLabel();
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setMinimumSize(640, 360);
    m_previewLabel->setStyleSheet("background-color: black;");
    mainLayout->addWidget(m_previewLabel);
    
    // Time info
    m_timeLabel = new QLabel();
    updateTimeLabel();
    mainLayout->addWidget(m_timeLabel);
    
    // Start slider
    QHBoxLayout* startLayout = new QHBoxLayout();
    startLayout->addWidget(new QLabel("Start:"));
    
    m_startSlider = new QSlider(Qt::Horizontal);
    m_startSlider->setMinimum(0);
    m_startSlider->setMaximum(m_totalFrames - 1);
    m_startSlider->setValue(0);
    connect(m_startSlider, &QSlider::valueChanged, this, &TrimDialog::onStartChanged);
    startLayout->addWidget(m_startSlider);
    
    m_startTimeLabel = new QLabel("0.0s");
    startLayout->addWidget(m_startTimeLabel);
    
    mainLayout->addLayout(startLayout);
    
    // End slider
    QHBoxLayout* endLayout = new QHBoxLayout();
    endLayout->addWidget(new QLabel("End:"));
    
    m_endSlider = new QSlider(Qt::Horizontal);
    m_endSlider->setMinimum(0);
    m_endSlider->setMaximum(m_totalFrames - 1);
    m_endSlider->setValue(m_totalFrames - 1);
    connect(m_endSlider, &QSlider::valueChanged, this, &TrimDialog::onEndChanged);
    endLayout->addWidget(m_endSlider);
    
    double duration = m_totalFrames / m_fps;
    m_endTimeLabel = new QLabel(QString("%1s").arg(duration, 0, 'f', 1));
    endLayout->addWidget(m_endTimeLabel);
    
    mainLayout->addLayout(endLayout);
    
    // Preview buttons
    QHBoxLayout* previewLayout = new QHBoxLayout();
    
    QPushButton* previewStartBtn = new QPushButton("Preview Start");
    connect(previewStartBtn, &QPushButton::clicked, this, &TrimDialog::onPreviewStart);
    previewLayout->addWidget(previewStartBtn);
    
    QPushButton* previewEndBtn = new QPushButton("Preview End");
    connect(previewEndBtn, &QPushButton::clicked, this, &TrimDialog::onPreviewEnd);
    previewLayout->addWidget(previewEndBtn);
    
    mainLayout->addLayout(previewLayout);
    
    // Dialog buttons
    QDialogButtonBox* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox);
}

void TrimDialog::onStartChanged(int value) {
    if (value >= m_endFrame) {
        value = m_endFrame - 1;
        m_startSlider->setValue(value);
    }
    
    m_startFrame = value;
    m_startTimeLabel->setText(QString("%1s").arg(value / m_fps, 0, 'f', 1));
    updateTimeLabel();
    showFrame(value);
}

void TrimDialog::onEndChanged(int value) {
    if (value <= m_startFrame) {
        value = m_startFrame + 1;
        m_endSlider->setValue(value);
    }
    
    m_endFrame = value;
    m_endTimeLabel->setText(QString("%1s").arg(value / m_fps, 0, 'f', 1));
    updateTimeLabel();
    showFrame(value);
}

void TrimDialog::onPreviewStart() {
    showFrame(m_startFrame);
}

void TrimDialog::onPreviewEnd() {
    showFrame(m_endFrame);
}

void TrimDialog::showFrame(int frameNum) {
    if (!m_capture.isOpened()) return;
    
    m_capture.set(cv::CAP_PROP_POS_FRAMES, frameNum);
    cv::Mat frame;
    
    if (m_capture.read(frame)) {
        cv::Mat rgb;
        cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);
        
        QImage img(rgb.data, rgb.cols, rgb.rows, rgb.step, QImage::Format_RGB888);
        QPixmap pixmap = QPixmap::fromImage(img);
        
        QPixmap scaled = pixmap.scaled(m_previewLabel->size(),
                                      Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation);
        m_previewLabel->setPixmap(scaled);
    }
}

void TrimDialog::updateTimeLabel() {
    double duration = (m_endFrame - m_startFrame) / m_fps;
    m_timeLabel->setText(QString("Trimmed Duration: %1s").arg(duration, 0, 'f', 1));
}