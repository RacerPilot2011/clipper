#ifndef TRIMDIALOG_H
#define TRIMDIALOG_H

#include <QDialog>
#include <QLabel>
#include <QSlider>
#include <QPushButton>
#include <QDialogButtonBox>
#include <opencv2/opencv.hpp>

/*
 * TrimDialog
 *
 * Purpose
 * - Presents a simple, non-destructive trimming UI for a single clip.
 * - Uses OpenCV to seek to requested frames for preview and allows the
 *   user to choose start/end frame indexes which the encoder will use to
 *   write a trimmed output.
 *
 * Implementation notes
 * - The dialog keeps only lightweight preview images in memory and relies
 *   on OpenCV's on-disk decoding for random access via `CAP_PROP_POS_FRAMES`.
 * - Frame numbers and FPS are used to display human-friendly durations.
 */

class TrimDialog : public QDialog {
    Q_OBJECT

public:
    explicit TrimDialog(const QString& videoPath, QWidget *parent = nullptr);
    ~TrimDialog();

    int getStartFrame() const { return m_startFrame; }
    int getEndFrame() const { return m_endFrame; }

private slots:
    void onStartChanged(int value);
    void onEndChanged(int value);
    void onPreviewStart();
    void onPreviewEnd();

private:
    void setupUI();
    void showFrame(int frameNum);
    void updateTimeLabel();

    QString m_videoPath;
    cv::VideoCapture m_capture;
    
    QLabel* m_previewLabel;
    QLabel* m_timeLabel;
    QLabel* m_startTimeLabel;
    QLabel* m_endTimeLabel;
    QSlider* m_startSlider;
    QSlider* m_endSlider;
    
    int m_startFrame;
    int m_endFrame;
    int m_totalFrames;
    double m_fps;
};
#endif // TRIMDIALOG_H