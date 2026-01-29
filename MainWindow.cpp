#include "MainWindow.h"
#include "TrimDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QFileDialog>
#include <QStandardPaths>
#include <QDir>
#include <QSettings>
#include <QInputDialog>
#include <QDesktopServices>
#include <QUrl>
#include <QProcess>
#include <QtConcurrent/QtConcurrent>
#include <QtGui/QCloseEvent>

#ifdef _WIN32
#include <windows.h>
#endif

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_hotkeyRegistered(false)
    , m_currentHotkey("F9")
#ifdef _WIN32
    , m_hotkeyId(1)
#endif
{
    // Initialize components
    m_screenRecorder = std::make_unique<ScreenRecorder>(30);
    m_micCapture = std::make_unique<AudioCapture>(AudioCapture::Microphone);
    m_desktopCapture = std::make_unique<AudioCapture>(AudioCapture::DesktopAudio);
    m_encoder = std::make_unique<VideoEncoder>();
    
    setupUI();
    setupConnections();
    loadSettings();
    loadClipsList();
    
    // Auto-start recording after UI is ready
    QTimer::singleShot(500, this, &MainWindow::autoStartRecording);
    QTimer::singleShot(1000, this, &MainWindow::registerGlobalHotkey);
}

MainWindow::~MainWindow() {
    unregisterGlobalHotkey();
}

void MainWindow::setupUI() {
    setWindowTitle("Screen Clip Recorder - C++ Edition");
    setGeometry(100, 100, 1200, 800);
    
    QWidget* central = new QWidget(this);
    setCentralWidget(central);
    
    QHBoxLayout* mainLayout = new QHBoxLayout(central);
    
    // Left panel
    QVBoxLayout* leftPanel = new QVBoxLayout();
    
    // Status
    m_statusLabel = new QLabel("Status: Starting...");
    leftPanel->addWidget(m_statusLabel);
    
    // Audio devices group
    QGroupBox* audioGroup = new QGroupBox("Audio Devices");
    QVBoxLayout* audioLayout = new QVBoxLayout(audioGroup);
    
    audioLayout->addWidget(new QLabel("Microphone:"));
    QHBoxLayout* micLayout = new QHBoxLayout();
    m_micCombo = new QComboBox();
    micLayout->addWidget(m_micCombo);
    audioLayout->addLayout(micLayout);
    
    audioLayout->addWidget(new QLabel("Desktop Audio:"));
    QHBoxLayout* desktopLayout = new QHBoxLayout();
    m_desktopCombo = new QComboBox();
    desktopLayout->addWidget(m_desktopCombo);
    audioLayout->addLayout(desktopLayout);
    
    m_applyAudioBtn = new QPushButton("Apply Audio Devices");
    audioLayout->addWidget(m_applyAudioBtn);
    
    leftPanel->addWidget(audioGroup);
    
    // Populate audio devices
    QStringList micDevices = AudioCapture::getAvailableDevices(AudioCapture::Microphone);
    m_micCombo->addItem("None", QVariant());
    for (const QString& dev : micDevices) {
        QStringList parts = dev.split('|');
        m_micCombo->addItem(parts[0], parts.size() > 1 ? parts[1] : QString());
    }
    
    QStringList desktopDevices = AudioCapture::getAvailableDevices(AudioCapture::DesktopAudio);
    m_desktopCombo->addItem("None", QVariant());
    for (const QString& dev : desktopDevices) {
        QStringList parts = dev.split('|');
        m_desktopCombo->addItem(parts[0], parts.size() > 1 ? parts[1] : QString());
    }
    
    // Buffer settings
    QGroupBox* bufferGroup = new QGroupBox("Replay Buffer");
    QVBoxLayout* bufferLayout = new QVBoxLayout(bufferGroup);
    
    m_bufferPreset = new QComboBox();
    m_bufferPreset->addItems({"15 seconds", "30 seconds", "1 minute", "2 minutes", "5 minutes", "Custom"});
    m_bufferPreset->setCurrentText("30 seconds");
    bufferLayout->addWidget(m_bufferPreset);
    
    m_customBufferWidget = new QWidget();
    QHBoxLayout* customLayout = new QHBoxLayout(m_customBufferWidget);
    customLayout->addWidget(new QLabel("Minutes:"));
    m_customMinutes = new QSpinBox();
    m_customMinutes->setRange(0, 10);
    customLayout->addWidget(m_customMinutes);
    customLayout->addWidget(new QLabel("Seconds:"));
    m_customSeconds = new QSpinBox();
    m_customSeconds->setRange(0, 59);
    m_customSeconds->setValue(30);
    customLayout->addWidget(m_customSeconds);
    m_customBufferWidget->setVisible(false);
    bufferLayout->addWidget(m_customBufferWidget);
    
    leftPanel->addWidget(bufferGroup);
    
    // Control buttons
    m_startStopBtn = new QPushButton("⏹️ Stop Recording");
    leftPanel->addWidget(m_startStopBtn);
    
    m_saveBtn = new QPushButton("💾 Save Clip (F9)");
    leftPanel->addWidget(m_saveBtn);
    
    m_hotkeyBtn = new QPushButton("⌨️ Hotkey: Enabled (F9)");
    leftPanel->addWidget(m_hotkeyBtn);
    
    // Hotkey customization
    QHBoxLayout* hotkeyLayout = new QHBoxLayout();
    hotkeyLayout->addWidget(new QLabel("Hotkey:"));
    m_hotkeyInput = new QLineEdit("F9");
    hotkeyLayout->addWidget(m_hotkeyInput);
    m_applyHotkeyBtn = new QPushButton("Apply");
    hotkeyLayout->addWidget(m_applyHotkeyBtn);
    leftPanel->addLayout(hotkeyLayout);
    
    m_uploadBtn = new QPushButton("☁️ Upload Selected");
    leftPanel->addWidget(m_uploadBtn);
    
    // Clips list
    leftPanel->addWidget(new QLabel("Saved Clips:"));
    m_clipsList = new QListWidget();
    leftPanel->addWidget(m_clipsList);
    
    // Clip actions
    QHBoxLayout* clipActions = new QHBoxLayout();
    m_trimBtn = new QPushButton("Trim");
    clipActions->addWidget(m_trimBtn);
    m_renameBtn = new QPushButton("Rename");
    clipActions->addWidget(m_renameBtn);
    m_deleteBtn = new QPushButton("Delete");
    clipActions->addWidget(m_deleteBtn);
    leftPanel->addLayout(clipActions);
    
    m_openFolderBtn = new QPushButton("Open Clips Folder");
    leftPanel->addWidget(m_openFolderBtn);
    
    mainLayout->addLayout(leftPanel, 1);
    
    // Right panel - video viewer
    m_clipViewer = new ClipViewer();
    mainLayout->addWidget(m_clipViewer, 2);
}

void MainWindow::setupConnections() {
    // Buttons
    connect(m_startStopBtn, &QPushButton::clicked, this, &MainWindow::onStartStopClicked);
    connect(m_saveBtn, &QPushButton::clicked, this, &MainWindow::onSaveClipClicked);
    connect(m_hotkeyBtn, &QPushButton::clicked, this, &MainWindow::onToggleHotkeyClicked);
    connect(m_applyAudioBtn, &QPushButton::clicked, this, &MainWindow::onApplyAudioDevices);
    connect(m_applyHotkeyBtn, &QPushButton::clicked, this, &MainWindow::onApplyHotkey);
    connect(m_uploadBtn, &QPushButton::clicked, this, &MainWindow::onUploadClip);
    connect(m_trimBtn, &QPushButton::clicked, this, &MainWindow::onTrimClip);
    connect(m_renameBtn, &QPushButton::clicked, this, &MainWindow::onRenameClip);
    connect(m_deleteBtn, &QPushButton::clicked, this, &MainWindow::onDeleteClip);
    connect(m_openFolderBtn, &QPushButton::clicked, this, &MainWindow::onOpenClipsFolder);
    
    // Clips list
    connect(m_clipsList, &QListWidget::itemClicked, this, &MainWindow::onClipSelected);
    
    // Buffer preset
    connect(m_bufferPreset, &QComboBox::currentTextChanged, this, &MainWindow::onBufferPresetChanged);
    
    // Recorder signals
    connect(m_screenRecorder.get(), &ScreenRecorder::recordingStarted, 
            this, &MainWindow::onRecordingStarted);
    connect(m_screenRecorder.get(), &ScreenRecorder::recordingStopped, 
            this, &MainWindow::onRecordingStopped);
    connect(m_screenRecorder.get(), &ScreenRecorder::errorOccurred, 
            this, &MainWindow::onErrorOccurred);
}

void MainWindow::autoStartRecording() {
    m_screenRecorder->startRecording();
    m_micCapture->startCapture();
    m_desktopCapture->startCapture();
}

void MainWindow::onStartStopClicked() {
    if (m_screenRecorder->isRecording()) {
        m_screenRecorder->stopRecording();
        m_micCapture->stopCapture();
        m_desktopCapture->stopCapture();
        m_startStopBtn->setText("▶️ Start Recording");
        m_saveBtn->setEnabled(false);
    } else {
        int bufferSecs = getBufferSeconds();
        m_screenRecorder->setBufferSeconds(bufferSecs);
        m_screenRecorder->startRecording();
        m_micCapture->startCapture();
        m_desktopCapture->startCapture();
        m_startStopBtn->setText("⏹️ Stop Recording");
        m_saveBtn->setEnabled(true);
    }
}

void MainWindow::onSaveClipClicked() {
    // This is called by hotkey or button
    int bufferSecs = getBufferSeconds();
    
    // Get frames and audio
    auto frames = m_screenRecorder->getFrames(bufferSecs);
    auto micAudio = m_micCapture->getBuffer(bufferSecs);
    auto desktopAudio = m_desktopCapture->getBuffer(bufferSecs);
    
    if (frames.empty()) {
        onErrorOccurred("No frames to save");
        return;
    }
    
    // Generate filename
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString filename = QString("clip_%1.mp4").arg(timestamp);
    QString filepath = getClipsDirectory() + "/" + filename;
    
    // Encode in background
    VideoEncoder::EncodeOptions options;
    options.outputPath = filepath;
    options.fps = m_screenRecorder->getFPS();
    
    onStatusUpdate("Encoding clip...");
    
    // Create a thread for encoding
    QtConcurrent::run([this, frames, micAudio, desktopAudio, options, filepath]() {
        bool success = m_encoder->encode(frames, micAudio, desktopAudio, options);
        
        if (success) {
            QMetaObject::invokeMethod(this, [this, filepath]() {
                onClipSaved(filepath);
            });
        }
    });
}

void MainWindow::onApplyAudioDevices() {
    // Stop current captures
    bool wasRecording = m_screenRecorder->isRecording();
    
    if (wasRecording) {
        m_micCapture->stopCapture();
        m_desktopCapture->stopCapture();
    }
    
    // Set new devices
    QString micDevice = m_micCombo->currentData().toString();
    QString desktopDevice = m_desktopCombo->currentData().toString();
    
    if (!micDevice.isEmpty()) {
        m_micCapture->setDevice(micDevice);
    }
    
    if (!desktopDevice.isEmpty()) {
        m_desktopCapture->setDevice(desktopDevice);
    }
    
    // Restart if was recording
    if (wasRecording) {
        m_micCapture->startCapture();
        m_desktopCapture->startCapture();
    }
    
    onStatusUpdate("Audio devices updated");
}

void MainWindow::onClipSaved(const QString& filepath) {
    onStatusUpdate("Clip saved successfully");
    loadClipsList();
    
    // Auto-select the new clip
    for (int i = 0; i < m_clipsList->count(); i++) {
        if (m_clipsList->item(i)->data(Qt::UserRole).toString() == filepath) {
            m_clipsList->setCurrentRow(i);
            m_clipViewer->loadClip(filepath);
            break;
        }
    }
}

void MainWindow::loadClipsList() {
    m_clipsList->clear();
    
    QDir clipsDir(getClipsDirectory());
    QStringList clips = clipsDir.entryList(QStringList() << "*.mp4", QDir::Files, QDir::Time);
    
    for (const QString& clip : clips) {
        QString fullPath = clipsDir.absoluteFilePath(clip);
        QListWidgetItem* item = new QListWidgetItem(clip);
        item->setData(Qt::UserRole, fullPath);
        m_clipsList->addItem(item);
    }
}

void MainWindow::onClipSelected(QListWidgetItem* item) {
    QString filepath = item->data(Qt::UserRole).toString();
    m_clipViewer->loadClip(filepath);
}

void MainWindow::onTrimClip() {
    QListWidgetItem* current = m_clipsList->currentItem();
    if (!current) {
        QMessageBox::information(this, "No Clip", "Please select a clip to trim");
        return;
    }
    
    QString filepath = current->data(Qt::UserRole).toString();
    
    TrimDialog dialog(filepath, this);
    if (dialog.exec() == QDialog::Accepted) {
        // TODO: Implement trim functionality
        onStatusUpdate("Trimming not yet implemented");
    }
}

void MainWindow::onRenameClip() {
    // TODO: Implement rename
    onStatusUpdate("Rename not yet implemented");
}

void MainWindow::onDeleteClip() {
    QListWidgetItem* current = m_clipsList->currentItem();
    if (!current) return;
    
    QString filepath = current->data(Qt::UserRole).toString();
    
    auto reply = QMessageBox::question(this, "Delete Clip",
                                       "Are you sure you want to delete this clip?");
    
    if (reply == QMessageBox::Yes) {
        if (m_clipViewer->currentClipPath() == filepath) {
            m_clipViewer->releaseCurrentClip();
        }
        
        QFile::remove(filepath);
        loadClipsList();
        onStatusUpdate("Clip deleted");
    }
}

void MainWindow::onUploadClip() {
    // TODO: Implement upload
    onStatusUpdate("Upload not yet implemented");
}

void MainWindow::onOpenClipsFolder() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(getClipsDirectory()));
}

void MainWindow::registerGlobalHotkey() {
#ifdef _WIN32
    // Register Windows hotkey
    RegisterHotKey((HWND)winId(), m_hotkeyId, 0, VK_F9);
    m_hotkeyRegistered = true;
    onStatusUpdate("Hotkey F9 registered");
#endif
}

void MainWindow::unregisterGlobalHotkey() {
#ifdef _WIN32
    if (m_hotkeyRegistered) {
        UnregisterHotKey((HWND)winId(), m_hotkeyId);
    }
#endif
}

void MainWindow::onApplyHotkey() {
    // TODO: Implement custom hotkey
    onStatusUpdate("Custom hotkeys not yet implemented");
}

void MainWindow::onToggleHotkeyClicked() {
    // TODO: Implement toggle
}

void MainWindow::onHotkeyTriggered() {
    onSaveClipClicked();
}

QString MainWindow::getClipsDirectory() const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/ScreenClips";
    QDir().mkpath(dir);
    return dir;
}

int MainWindow::getBufferSeconds() const {
    QString preset = m_bufferPreset->currentText();
    
    if (preset == "15 seconds") return 15;
    if (preset == "30 seconds") return 30;
    if (preset == "1 minute") return 60;
    if (preset == "2 minutes") return 120;
    if (preset == "5 minutes") return 300;
    if (preset == "Custom") {
        return m_customMinutes->value() * 60 + m_customSeconds->value();
    }
    
    return 30;
}

void MainWindow::onBufferPresetChanged(const QString& preset) {
    m_customBufferWidget->setVisible(preset == "Custom");
}

void MainWindow::onRecordingStarted() {
    onStatusUpdate("Recording started");
}

void MainWindow::onRecordingStopped() {
    onStatusUpdate("Recording stopped");
}

void MainWindow::onStatusUpdate(const QString& message) {
    m_statusLabel->setText("Status: " + message);
}

void MainWindow::onErrorOccurred(const QString& error) {
    QMessageBox::critical(this, "Error", error);
}

void MainWindow::loadSettings() {
    QSettings settings("ScreenClip", "Recorder");
    m_username = settings.value("username", "Anonymous").toString();
}

void MainWindow::saveSettings() {
    QSettings settings("ScreenClip", "Recorder");
    settings.setValue("username", m_username);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    m_clipViewer->releaseCurrentClip();
    m_screenRecorder->stopRecording();
    m_micCapture->stopCapture();
    m_desktopCapture->stopCapture();
    unregisterGlobalHotkey();
    saveSettings();
    event->accept();
}

#ifdef _WIN32
bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result) {
    MSG* msg = static_cast<MSG*>(message);
    
    if (msg->message == WM_HOTKEY && msg->wParam == m_hotkeyId) {
        onHotkeyTriggered();
        return true;
    }
    
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif