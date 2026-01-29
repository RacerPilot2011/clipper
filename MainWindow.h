#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QSlider>
#include <QSpinBox>
#include <QLineEdit>
#include <QTimer>
#include <memory>
#include "ScreenRecorder.h"
#include "AudioCapture.h"
#include "ClipViewer.h"
#include "VideoEncoder.h"

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    // Recording controls
    void onStartStopClicked();
    void onSaveClipClicked();
    void onToggleHotkeyClicked();
    
    // Audio device selection
    void onApplyAudioDevices();
    void onMicDeviceChanged(int index);
    void onDesktopDeviceChanged(int index);
    
    // Buffer settings
    void onBufferPresetChanged(const QString& preset);
    void onCustomBufferChanged();
    
    // Clip management
    void onClipSelected(QListWidgetItem* item);
    void onTrimClip();
    void onRenameClip();
    void onDeleteClip();
    void onUploadClip();
    void onOpenClipsFolder();
    
    // Hotkey
    void onApplyHotkey();
    void onHotkeyTriggered();
    
    // Recording signals
    void onRecordingStarted();
    void onRecordingStopped();
    void onClipSaved(const QString& filepath);
    
    // Status updates
    void onStatusUpdate(const QString& message);
    void onErrorOccurred(const QString& error);

private:
    void setupUI();
    void setupConnections();
    void loadClipsList();
    void loadSettings();
    void saveSettings();
    void autoStartRecording();
    void registerGlobalHotkey();
    void unregisterGlobalHotkey();
    
    QString getClipsDirectory() const;
    int getBufferSeconds() const;
    
    // Core components
    std::unique_ptr<ScreenRecorder> m_screenRecorder;
    std::unique_ptr<AudioCapture> m_micCapture;
    std::unique_ptr<AudioCapture> m_desktopCapture;
    std::unique_ptr<VideoEncoder> m_encoder;
    
    // UI Components
    QLabel* m_statusLabel;
    
    // Audio selection
    QComboBox* m_micCombo;
    QComboBox* m_desktopCombo;
    QPushButton* m_applyAudioBtn;
    
    // Buffer settings
    QComboBox* m_bufferPreset;
    QWidget* m_customBufferWidget;
    QSpinBox* m_customMinutes;
    QSpinBox* m_customSeconds;
    
    // Control buttons
    QPushButton* m_startStopBtn;
    QPushButton* m_saveBtn;
    QPushButton* m_hotkeyBtn;
    QPushButton* m_uploadBtn;
    
    // Hotkey settings
    QLineEdit* m_hotkeyInput;
    QPushButton* m_applyHotkeyBtn;
    
    // Clip list and viewer
    QListWidget* m_clipsList;
    ClipViewer* m_clipViewer;
    
    // Clip actions
    QPushButton* m_trimBtn;
    QPushButton* m_renameBtn;
    QPushButton* m_deleteBtn;
    QPushButton* m_openFolderBtn;
    
    // State
    bool m_hotkeyRegistered;
    QString m_username;
    QString m_currentHotkey;
    
#ifdef _WIN32
    // Windows hotkey registration
    int m_hotkeyId;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#endif
};

#endif // MAINWINDOW_H