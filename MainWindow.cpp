/*
 * MainWindow.cpp
 *
 * Implements the primary application window and coordinates between the
 * UI controls and backend subsystems (screen capture, audio capture and
 * encoding). All user interaction flows (start/stop, save, trim, device
 * selection) are handled here.
 */

#include "MainWindow.h"
#include "TrimDialog.h"
#include "EncoderWorker.h"
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
#include <QThread>
#include <QThreadPool>
#include <QCloseEvent>
#include <QProgressDialog>

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
    
    // Debug log viewer
    QGroupBox* logGroup = new QGroupBox("Debug Log");
    QVBoxLayout* logLayout = new QVBoxLayout(logGroup);
    
    m_logViewer = new QTextEdit();
    m_logViewer->setReadOnly(true);
    m_logViewer->setMaximumHeight(150);
    m_logViewer->setStyleSheet("QTextEdit { font-family: 'Consolas', 'Courier New', monospace; font-size: 8pt; background-color: #1e1e1e; color: #d4d4d4; }");
    logLayout->addWidget(m_logViewer);
    
    QHBoxLayout* logBtnLayout = new QHBoxLayout();
    QPushButton* clearLogBtn = new QPushButton("Clear");
    clearLogBtn->setMaximumWidth(80);
    connect(clearLogBtn, &QPushButton::clicked, m_logViewer, &QTextEdit::clear);
    logBtnLayout->addWidget(clearLogBtn);
    logBtnLayout->addStretch();
    logLayout->addLayout(logBtnLayout);
    
    leftPanel->addWidget(logGroup);
    
    addLog("🚀 Application started");
    
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
    connect(m_screenRecorder.get(), &ScreenRecorder::debugLog,
            this, &MainWindow::addLog);
}

void MainWindow::autoStartRecording() {
    // Set default audio devices if available
    if (m_micCombo->count() > 1) {
        m_micCombo->setCurrentIndex(1); // First non-"None" device
        QString micDevice = m_micCombo->currentData().toString();
        if (!micDevice.isEmpty()) {
            m_micCapture->setDevice(micDevice);
        }
    }
    
    if (m_desktopCombo->count() > 1) {
        m_desktopCombo->setCurrentIndex(1); // First non-"None" device
        QString desktopDevice = m_desktopCombo->currentData().toString();
        if (!desktopDevice.isEmpty()) {
            m_desktopCapture->setDevice(desktopDevice);
        }
    }
    
    // Start recording
    m_screenRecorder->startRecording();
    m_micCapture->startCapture();
    m_desktopCapture->startCapture();
    
    addLog("🎤 Mic capture started");
    addLog("🔊 Desktop audio capture started");
}

void MainWindow::onStartStopClicked() {
    if (m_screenRecorder->isRecording()) {
        m_screenRecorder->stopRecording();
        m_micCapture->stopCapture();
        m_desktopCapture->stopCapture();
        m_startStopBtn->setText("▶️ Start Recording");
        m_saveBtn->setEnabled(false);
        addLog("⏹️ All recording stopped");
    } else {
        int bufferSecs = getBufferSeconds();
        m_screenRecorder->setBufferSeconds(bufferSecs);
        m_screenRecorder->startRecording();
        m_micCapture->startCapture();
        m_desktopCapture->startCapture();
        m_startStopBtn->setText("⏹️ Stop Recording");
        m_saveBtn->setEnabled(true);
        addLog(QString("▶️ All recording started (buffer: %1s)").arg(bufferSecs));
    }
}

void MainWindow::onSaveClipClicked() {
    // Disable save button to prevent double-clicks
    m_saveBtn->setEnabled(false);
    
    int bufferSecs = getBufferSeconds();
    
    addLog(QString("💾 SAVE CLIP REQUESTED (%1 seconds)").arg(bufferSecs));
    
    // Get frames and audio
    auto frames = m_screenRecorder->getFrames(bufferSecs);
    auto micAudio = m_micCapture->getBuffer(bufferSecs);
    auto desktopAudio = m_desktopCapture->getBuffer(bufferSecs);
    
    addLog(QString("📊 Retrieved: %1 frames, %2 mic chunks, %3 desktop chunks")
        .arg(frames.size())
        .arg(micAudio.size())
        .arg(desktopAudio.size()));
    
    if (frames.empty()) {
        QString error = "❌ No frames to save - recording might not be started";
        onErrorOccurred(error);
        addLog(error);
        m_saveBtn->setEnabled(true);
        return;
    }
    
    // Calculate actual duration
    double actualDuration = frames.size() / (double)m_screenRecorder->getFPS();
    addLog(QString("⏱️  Duration: %1 seconds at %2 fps")
        .arg(actualDuration, 0, 'f', 1)
        .arg(m_screenRecorder->getFPS()));
    
    if (actualDuration < 1.0) {
        QString warning = QString("⚠️  Warning: Very short clip (%1s)").arg(actualDuration, 0, 'f', 1);
        addLog(warning);
        QMessageBox::warning(this, "Short Clip", 
            QString("Clip is only %1 seconds long. Buffer may need more time to fill.").arg(actualDuration, 0, 'f', 1));
    }
    
    // Generate filename
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString filename = QString("clip_%1.mp4").arg(timestamp);
    QString filepath = getClipsDirectory() + "/" + filename;
    
    addLog(QString("📁 Output: %1").arg(filepath));
    
    // Set encoding options
    VideoEncoder::EncodeOptions options;
    options.outputPath = filepath;
    options.fps = m_screenRecorder->getFPS();
    options.audioSampleRate = 48000; // Standard sample rate
    
    onStatusUpdate("Encoding clip...");
    addLog("🎬 Starting encoder...");
    
    // Create progress dialog
    QProgressDialog* progressDialog = new QProgressDialog("Encoding video...", "Cancel", 0, 100, this);
    progressDialog->setWindowModality(Qt::WindowModal);
    progressDialog->setMinimumDuration(0);
    progressDialog->setValue(0);
    
    QThread* thread = new QThread;
    EncoderWorker* worker = new EncoderWorker(
        m_encoder.get(),
        std::move(frames),
        std::move(micAudio),
        std::move(desktopAudio),
        options
    );
    
    worker->moveToThread(thread);
    
    // Connect signals
    connect(thread, &QThread::started, worker, &EncoderWorker::process);
    connect(worker, &EncoderWorker::finished, thread, &QThread::quit);
    connect(worker, &EncoderWorker::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    
    // Progress updates
    connect(worker, &EncoderWorker::progressUpdate, progressDialog, &QProgressDialog::setValue);
    connect(worker, &EncoderWorker::progressUpdate, this, [this](int percent) {
        addLog(QString("📊 Encoding progress: %1%").arg(percent));
    });
    
    // Completion
    connect(worker, &EncoderWorker::encodingComplete, this, [this, filepath, progressDialog](bool success, const QString& message) {
        progressDialog->close();
        
        if (success) {
            addLog(QString("✅ %1").arg(message));
            onClipSaved(filepath);
        } else {
            addLog(QString("❌ Encoding failed: %1").arg(message));
            onErrorOccurred("Failed to encode clip");
        }
        
        m_saveBtn->setEnabled(true);
    });
    
    // Errors
    connect(worker, &EncoderWorker::errorOccurred, this, [this, progressDialog](const QString& error) {
        addLog(QString("❌ Encoder error: %1").arg(error));
        progressDialog->close();
        m_saveBtn->setEnabled(true);
    });
    
    // Cancel button
    connect(progressDialog, &QProgressDialog::canceled, this, [this, thread]() {
        addLog("⚠️ Encoding canceled by user");
        thread->quit();
        m_saveBtn->setEnabled(true);
    });
    
    // Start encoding
    thread->start();
    addLog("🔄 Encoder thread started");
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
    
    bool micSet = false;
    bool desktopSet = false;
    
    if (!micDevice.isEmpty()) {
        if (m_micCapture->setDevice(micDevice)) {
            micSet = true;
            addLog(QString("🎤 Mic device set: %1").arg(m_micCombo->currentText()));
        } else {
            addLog("❌ Failed to set mic device");
        }
    }
    
    if (!desktopDevice.isEmpty()) {
        if (m_desktopCapture->setDevice(desktopDevice)) {
            desktopSet = true;
            addLog(QString("🔊 Desktop device set: %1").arg(m_desktopCombo->currentText()));
        } else {
            addLog("❌ Failed to set desktop device");
        }
    }
    
    // Restart if was recording
    if (wasRecording) {
        if (micSet) m_micCapture->startCapture();
        if (desktopSet) m_desktopCapture->startCapture();
    }
    
    onStatusUpdate("Audio devices updated");
}

void MainWindow::onClipSaved(const QString& filepath) {
    onStatusUpdate("Clip saved successfully");
    loadClipsList();
    
    QFileInfo info(filepath);
    addLog(QString("💾 Clip saved: %1 (%2 MB)")
        .arg(info.fileName())
        .arg(info.size() / 1024.0 / 1024.0, 0, 'f', 2));
    
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
    
    addLog(QString("📂 Found %1 clips in folder").arg(clips.size()));
    
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
    addLog(QString("▶️ Playing: %1").arg(item->text()));
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
        addLog(QString("🗑️ Deleted: %1").arg(current->text()));
    }
}

void MainWindow::onUploadClip() {
    // TODO: Implement upload
    onStatusUpdate("Upload not yet implemented");
}

void MainWindow::onOpenClipsFolder() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(getClipsDirectory()));
    addLog(QString("📂 Opened clips folder: %1").arg(getClipsDirectory()));
}

void MainWindow::registerGlobalHotkey() {
#ifdef _WIN32
    // Register Windows hotkey
    if (RegisterHotKey((HWND)winId(), m_hotkeyId, 0, VK_F9)) {
        m_hotkeyRegistered = true;
        onStatusUpdate("Hotkey F9 registered");
        addLog("⌨️ Global hotkey F9 registered");
    } else {
        addLog("⚠️ Failed to register F9 hotkey - may already be in use");
    }
#else
    addLog("⚠️ Global hotkeys not supported on this platform");
#endif
}

void MainWindow::unregisterGlobalHotkey() {
#ifdef _WIN32
    if (m_hotkeyRegistered) {
        UnregisterHotKey((HWND)winId(), m_hotkeyId);
        addLog("⌨️ Global hotkey unregistered");
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
    addLog("⌨️ Hotkey F9 triggered!");
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

void MainWindow::addLog(const QString& message) {
    if (!m_logViewer) return;
    
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss");
    QString logLine = QString("[%1] %2").arg(timestamp, message);
    m_logViewer->append(logLine);
    
    // Auto-scroll to bottom
    QTextCursor cursor = m_logViewer->textCursor();
    cursor.movePosition(QTextCursor::End);
    m_logViewer->setTextCursor(cursor);
    
    // Keep log manageable (max 200 lines)
    QTextDocument* doc = m_logViewer->document();
    if (doc->blockCount() > 200) {
        cursor.movePosition(QTextCursor::Start);
        cursor.select(QTextCursor::BlockUnderCursor);
        cursor.removeSelectedText();
        cursor.deleteChar(); // Remove the newline
    }
}

void MainWindow::onBufferPresetChanged(const QString& preset) {
    m_customBufferWidget->setVisible(preset == "Custom");
    int bufferSecs = getBufferSeconds();
    m_screenRecorder->setBufferSeconds(bufferSecs);
    addLog(QString("⚙️ Buffer changed to %1 seconds").arg(bufferSecs));
}

void MainWindow::onRecordingStarted() {
    onStatusUpdate("Recording started");
    addLog("🎥 Screen recording started");
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
    addLog("🛑 Application closing...");
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