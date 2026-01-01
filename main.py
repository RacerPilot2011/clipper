import sys
import cv2
import numpy as np
import threading
import time
from datetime import datetime
from collections import deque
from pathlib import Path
import mss
from PyQt6.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout, 
                             QHBoxLayout, QPushButton, QLabel, QListWidget, 
                             QMessageBox, QFileDialog, QSlider, QSpinBox,
                             QDialog, QDialogButtonBox, QLineEdit, QInputDialog)
from PyQt6.QtCore import QTimer, Qt, pyqtSignal, QObject
from PyQt6.QtGui import QPixmap, QImage
import keyboard
import requests
import json
from urllib.parse import urlparse
import platform
import os

class RecorderSignals(QObject):
    """Signals for thread-safe communication"""
    clip_saved = pyqtSignal(str)
    status_update = pyqtSignal(str)
    error_occurred = pyqtSignal(str)
    permission_needed = pyqtSignal()

class ScreenRecorder:
    """Handles continuous screen recording and clip saving"""
    
    def __init__(self, buffer_seconds=30, fps=30):
        self.buffer_seconds = buffer_seconds
        self.fps = fps
        self.frame_buffer = deque(maxlen=buffer_seconds * fps)
        self.is_recording = False
        self.record_thread = None
        self.signals = RecorderSignals()
        self.clips_dir = Path.home() / "ScreenClips"
        self.clips_dir.mkdir(exist_ok=True)
        self.permission_granted = False
        self.sct = None
        
    def check_screen_recording_permission(self):
        """Check if screen recording permission is granted"""
        if platform.system() != "Darwin":  # Not macOS
            # For Linux, check if DISPLAY is set
            if platform.system() == "Linux":
                if not os.environ.get('DISPLAY'):
                    return False
            return True
        
        try:
            # Try to capture a small screenshot to test permissions
            with mss.mss() as sct:
                monitor = sct.monitors[1]
                test_region = {
                    "top": monitor["top"],
                    "left": monitor["left"],
                    "width": 1,
                    "height": 1
                }
                screenshot = sct.grab(test_region)
                
                self.permission_granted = True
                return True
        except Exception as e:
            self.permission_granted = False
            return False
    
    def start_recording(self):
        """Start continuous buffer recording"""
        # Check permissions first on macOS
        if platform.system() == "Darwin" and not self.permission_granted:
            if not self.check_screen_recording_permission():
                self.signals.permission_needed.emit()
                return False
        
        if not self.is_recording:
            self.is_recording = True
            self.record_thread = threading.Thread(target=self._record_loop, daemon=True)
            self.record_thread.start()
            self.signals.status_update.emit("Recording buffer active")
            return True
        return True
    
    def stop_recording(self):
        """Stop buffer recording"""
        self.is_recording = False
        if self.record_thread:
            self.record_thread.join(timeout=2)
        
        # Close mss instance
        if self.sct:
            try:
                self.sct.close()
            except:
                pass
            self.sct = None
        
        self.signals.status_update.emit("Recording stopped")
    
    def _record_loop(self):
        """Continuously capture screen to buffer"""
        try:
            # Create persistent mss instance for this thread
            self.sct = mss.mss()
            
            # Get monitor info
            monitor = self.sct.monitors[1]  # Primary monitor
            
            # For Linux, ensure we're capturing the full screen correctly
            if platform.system() == "Linux":
                # Sometimes mss needs explicit monitor bounds
                monitor = {
                    "top": 0,
                    "left": 0,
                    "width": monitor["width"],
                    "height": monitor["height"],
                    "mon": 1
                }
            
            frame_delay = 1.0 / self.fps
            
            while self.is_recording:
                start_time = time.time()
                
                try:
                    # Capture screen with error handling
                    screenshot = self.sct.grab(monitor)
                    
                    # Convert to numpy array
                    frame = np.array(screenshot)
                    
                    # Convert BGRA to BGR
                    if frame.shape[2] == 4:
                        frame = cv2.cvtColor(frame, cv2.COLOR_BGRA2BGR)
                    elif frame.shape[2] == 3:
                        frame = cv2.cvtColor(frame, cv2.COLOR_RGB2BGR)
                    
                    # Add timestamp
                    timestamp = datetime.now()
                    self.frame_buffer.append((frame, timestamp))
                    
                except Exception as e:
                    error_msg = str(e)
                    
                    if "XGetImage" in error_msg or "X11" in error_msg:
                        self.signals.error_occurred.emit(
                            "X11 capture error. Try:\n"
                            "1. Run: xhost +local:\n"
                            "2. Or set DISPLAY variable\n"
                            "3. Check if running in Wayland (may need XWayland)"
                        )
                        self.is_recording = False
                        break
                    elif platform.system() == "Darwin":
                        # Permission was revoked
                        self.signals.error_occurred.emit("Screen recording permission lost")
                        self.is_recording = False
                        self.permission_granted = False
                        self.signals.permission_needed.emit()
                        break
                    else:
                        # Other error, but try to continue
                        print(f"Frame capture error: {e}")
                        time.sleep(0.5)
                        continue
                
                # Maintain target FPS
                elapsed = time.time() - start_time
                sleep_time = max(0, frame_delay - elapsed)
                time.sleep(sleep_time)
            
            # Clean up mss instance
            if self.sct:
                try:
                    self.sct.close()
                except:
                    pass
                self.sct = None
                    
        except Exception as e:
            self.signals.error_occurred.emit(f"Recording error: {str(e)}")
            self.is_recording = False
            
            # Clean up on error
            if self.sct:
                try:
                    self.sct.close()
                except:
                    pass
                self.sct = None
    
    def save_clip(self, duration_seconds=None):
        """Save the last N seconds from buffer"""
        if not self.frame_buffer:
            self.signals.error_occurred.emit("No frames in buffer")
            return None
        
        if duration_seconds is None:
            duration_seconds = self.buffer_seconds
        
        # Calculate how many frames to save
        frames_to_save = min(int(duration_seconds * self.fps), len(self.frame_buffer))
        
        if frames_to_save == 0:
            self.signals.error_occurred.emit("Not enough frames")
            return None
        
        # Get frames from end of buffer
        frames_list = list(self.frame_buffer)[-frames_to_save:]
        
        # Generate filename with metadata
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        readable_time = datetime.now().strftime("%Y-%m-%d %I:%M:%S %p")
        filename = self.clips_dir / f"clip_{timestamp}.mp4"
        metadata_file = self.clips_dir / f"clip_{timestamp}.meta"
        
        # Save video in background thread
        threading.Thread(target=self._save_video, 
                        args=(frames_list, filename, metadata_file, readable_time), 
                        daemon=True).start()
        
        return str(filename)
    
    def _save_video(self, frames_list, filename, metadata_file, readable_time):
        """Save frames to video file"""
        try:
            if not frames_list:
                return
            
            # Get frame dimensions
            height, width = frames_list[0][0].shape[:2]
            
            # Create video writer
            fourcc = cv2.VideoWriter_fourcc(*'mp4v')
            out = cv2.VideoWriter(str(filename), fourcc, self.fps, (width, height))
            
            # Write frames
            for frame, _ in frames_list:
                out.write(frame)
            
            out.release()
            
            # Save metadata file with timestamp and custom name
            with open(metadata_file, 'w') as f:
                f.write(f"timestamp={readable_time}\n")
                f.write(f"name=\n")  # Empty name initially
            
            self.signals.clip_saved.emit(str(filename))
            self.signals.status_update.emit(f"Clip saved: {filename.name}")
            
        except Exception as e:
            self.signals.error_occurred.emit(f"Error saving clip: {str(e)}")

class TrimDialog(QDialog):
    """Dialog for trimming video clips"""
    
    def __init__(self, video_path, parent=None):
        super().__init__(parent)
        self.video_path = video_path
        self.cap = cv2.VideoCapture(video_path)
        self.total_frames = int(self.cap.get(cv2.CAP_PROP_FRAME_COUNT))
        self.fps = self.cap.get(cv2.CAP_PROP_FPS)
        self.duration = self.total_frames / self.fps
        
        self.start_frame = 0
        self.end_frame = self.total_frames - 1
        
        self.init_ui()
        
    def init_ui(self):
        self.setWindowTitle("Trim Clip")
        self.setGeometry(200, 200, 800, 600)
        
        layout = QVBoxLayout()
        
        # Video preview
        self.preview_label = QLabel()
        self.preview_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.preview_label.setMinimumSize(640, 360)
        self.preview_label.setStyleSheet("background-color: black;")
        layout.addWidget(self.preview_label)
        
        # Timeline info
        self.time_label = QLabel()
        self.update_time_label()
        layout.addWidget(self.time_label)
        
        # Start trim slider
        start_layout = QHBoxLayout()
        start_layout.addWidget(QLabel("Start:"))
        self.start_slider = QSlider(Qt.Orientation.Horizontal)
        self.start_slider.setMinimum(0)
        self.start_slider.setMaximum(self.total_frames - 1)
        self.start_slider.setValue(0)
        self.start_slider.valueChanged.connect(self.on_start_changed)
        start_layout.addWidget(self.start_slider)
        self.start_time_label = QLabel("0.0s")
        start_layout.addWidget(self.start_time_label)
        layout.addLayout(start_layout)
        
        # End trim slider
        end_layout = QHBoxLayout()
        end_layout.addWidget(QLabel("End:"))
        self.end_slider = QSlider(Qt.Orientation.Horizontal)
        self.end_slider.setMinimum(0)
        self.end_slider.setMaximum(self.total_frames - 1)
        self.end_slider.setValue(self.total_frames - 1)
        self.end_slider.valueChanged.connect(self.on_end_changed)
        end_layout.addWidget(self.end_slider)
        self.end_time_label = QLabel(f"{self.duration:.1f}s")
        end_layout.addWidget(self.end_time_label)
        layout.addLayout(end_layout)
        
        # Preview buttons
        preview_layout = QHBoxLayout()
        preview_start_btn = QPushButton("Preview Start")
        preview_start_btn.clicked.connect(lambda: self.show_frame(self.start_frame))
        preview_layout.addWidget(preview_start_btn)
        
        preview_end_btn = QPushButton("Preview End")
        preview_end_btn.clicked.connect(lambda: self.show_frame(self.end_frame))
        preview_layout.addWidget(preview_end_btn)
        layout.addLayout(preview_layout)
        
        # Dialog buttons
        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Save | 
            QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        
        self.setLayout(layout)
        
        # Show first frame
        self.show_frame(0)
    
    def on_start_changed(self, value):
        """Handle start slider change"""
        if value >= self.end_frame:
            value = self.end_frame - 1
            self.start_slider.setValue(value)
        
        self.start_frame = value
        self.start_time_label.setText(f"{value / self.fps:.1f}s")
        self.update_time_label()
        self.show_frame(value)
    
    def on_end_changed(self, value):
        """Handle end slider change"""
        if value <= self.start_frame:
            value = self.start_frame + 1
            self.end_slider.setValue(value)
        
        self.end_frame = value
        self.end_time_label.setText(f"{value / self.fps:.1f}s")
        self.update_time_label()
        self.show_frame(value)
    
    def update_time_label(self):
        """Update duration label"""
        duration = (self.end_frame - self.start_frame) / self.fps
        self.time_label.setText(f"Trimmed Duration: {duration:.1f}s")
    
    def show_frame(self, frame_num):
        """Display specific frame"""
        self.cap.set(cv2.CAP_PROP_POS_FRAMES, frame_num)
        ret, frame = self.cap.read()
        
        if ret:
            # Convert BGR to RGB
            rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            
            # Convert to QImage
            h, w, ch = rgb_frame.shape
            bytes_per_line = ch * w
            q_img = QImage(rgb_frame.data, w, h, bytes_per_line, QImage.Format.Format_RGB888)
            
            # Scale to fit label
            pixmap = QPixmap.fromImage(q_img)
            scaled_pixmap = pixmap.scaled(self.preview_label.size(), 
                                          Qt.AspectRatioMode.KeepAspectRatio,
                                          Qt.TransformationMode.SmoothTransformation)
            self.preview_label.setPixmap(scaled_pixmap)
    
    def get_trim_range(self):
        """Get selected trim range"""
        return self.start_frame, self.end_frame
    
    def closeEvent(self, event):
        """Clean up on close"""
        if self.cap:
            self.cap.release()
        event.accept()

class ClipViewer(QWidget):
    """Widget for viewing saved clips"""
    
    def __init__(self):
        super().__init__()
        self.current_clip = None
        self.current_clip_path = None
        self.cap = None
        self.init_ui()
        
    def init_ui(self):
        layout = QVBoxLayout()
        
        # Video display
        self.video_label = QLabel("No clip loaded")
        self.video_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.video_label.setMinimumSize(640, 480)
        self.video_label.setStyleSheet("background-color: black; color: white;")
        layout.addWidget(self.video_label)
        
        # Time label
        self.time_label = QLabel("0:00 / 0:00")
        self.time_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self.time_label)
        
        # Controls
        controls = QHBoxLayout()
        self.play_btn = QPushButton("Play")
        self.play_btn.clicked.connect(self.toggle_play)
        self.play_btn.setEnabled(False)
        controls.addWidget(self.play_btn)
        
        self.position_slider = QSlider(Qt.Orientation.Horizontal)
        self.position_slider.setEnabled(False)
        self.position_slider.sliderMoved.connect(self.seek)
        controls.addWidget(self.position_slider)
        
        layout.addLayout(controls)
        
        # Timer for playback
        self.timer = QTimer()
        self.timer.timeout.connect(self.update_frame)
        self.is_playing = False
        
        self.setLayout(layout)
    
    def release_current_clip(self):
        """Release current video capture"""
        if self.is_playing:
            self.is_playing = False
            self.timer.stop()
            self.play_btn.setText("Play")
        
        if self.cap:
            self.cap.release()
            self.cap = None
        
        # Force Python to close the file handle
        import gc
        gc.collect()
        
        # Clear display
        self.video_label.clear()
        self.video_label.setText("No clip loaded")
        self.time_label.setText("0:00 / 0:00")
        self.position_slider.setValue(0)
        self.play_btn.setEnabled(False)
        self.position_slider.setEnabled(False)
        
        self.current_clip_path = None
    
    def load_clip(self, filepath):
        """Load a video clip"""
        # Release previous clip first
        self.release_current_clip()
        
        self.current_clip_path = filepath
        self.cap = cv2.VideoCapture(filepath)
        
        if self.cap.isOpened():
            self.play_btn.setEnabled(True)
            self.position_slider.setEnabled(True)
            
            # Set slider range
            total_frames = int(self.cap.get(cv2.CAP_PROP_FRAME_COUNT))
            self.position_slider.setMaximum(total_frames - 1)
            
            # Show first frame
            self.show_frame(0)
            self.update_time_display()
        
    def toggle_play(self):
        """Play/pause video"""
        if not self.cap:
            return
        
        self.is_playing = not self.is_playing
        
        if self.is_playing:
            self.play_btn.setText("Pause")
            fps = self.cap.get(cv2.CAP_PROP_FPS)
            self.timer.start(int(1000 / fps))
        else:
            self.play_btn.setText("Play")
            self.timer.stop()
    
    def update_frame(self):
        """Update video frame during playback"""
        if not self.cap:
            return
        
        ret, frame = self.cap.read()
        
        if ret:
            self.display_frame(frame)
            current_pos = int(self.cap.get(cv2.CAP_PROP_POS_FRAMES))
            self.position_slider.setValue(current_pos)
            self.update_time_display()
        else:
            # End of video
            self.is_playing = False
            self.play_btn.setText("Play")
            self.timer.stop()
            self.cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
            self.position_slider.setValue(0)
    
    def show_frame(self, frame_num):
        """Show specific frame"""
        if not self.cap:
            return
        
        self.cap.set(cv2.CAP_PROP_POS_FRAMES, frame_num)
        ret, frame = self.cap.read()
        
        if ret:
            self.display_frame(frame)
    
    def seek(self, position):
        """Seek to position"""
        self.show_frame(position)
        self.update_time_display()
    
    def display_frame(self, frame):
        """Display frame in label"""
        # Convert BGR to RGB
        rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        
        # Convert to QImage
        h, w, ch = rgb_frame.shape
        bytes_per_line = ch * w
        q_img = QImage(rgb_frame.data, w, h, bytes_per_line, QImage.Format.Format_RGB888)
        
        # Scale to fit label
        pixmap = QPixmap.fromImage(q_img)
        scaled_pixmap = pixmap.scaled(self.video_label.size(), 
                                      Qt.AspectRatioMode.KeepAspectRatio,
                                      Qt.TransformationMode.SmoothTransformation)
        self.video_label.setPixmap(scaled_pixmap)
    
    def update_time_display(self):
        """Update time display"""
        if not self.cap:
            return
        
        current_frame = int(self.cap.get(cv2.CAP_PROP_POS_FRAMES))
        total_frames = int(self.cap.get(cv2.CAP_PROP_FRAME_COUNT))
        fps = self.cap.get(cv2.CAP_PROP_FPS)
        
        current_time = current_frame / fps if fps > 0 else 0
        total_time = total_frames / fps if fps > 0 else 0
        
        current_str = f"{int(current_time // 60)}:{int(current_time % 60):02d}"
        total_str = f"{int(total_time // 60)}:{int(total_time % 60):02d}"
        
        self.time_label.setText(f"{current_str} / {total_str}")

class MainWindow(QMainWindow):
    """Main application window"""
    
    def __init__(self):
        super().__init__()
        self.recorder = ScreenRecorder(buffer_seconds=30, fps=30)
        self.hotkey_registered = False
        self.init_ui()
        self.setup_signals()
        self.load_clips_list()
        
        # Auto-start recording after checking permissions
        QTimer.singleShot(500, self.auto_start_recording)
        
        # Auto-enable hotkey
        QTimer.singleShot(1000, self.auto_enable_hotkey)
        
    def init_ui(self):
        self.setWindowTitle("Screen Clip Recorder")
        self.setGeometry(100, 100, 1000, 700)
        
        # Central widget
        central = QWidget()
        self.setCentralWidget(central)
        
        # Main layout
        main_layout = QHBoxLayout()
        
        # Left panel - Controls and clip list
        left_panel = QVBoxLayout()
        
        # Status
        self.status_label = QLabel("Status: Starting...")
        left_panel.addWidget(self.status_label)
        
        # Buffer duration setting
        buffer_layout = QHBoxLayout()
        buffer_layout.addWidget(QLabel("Buffer (seconds):"))
        self.buffer_spin = QSpinBox()
        self.buffer_spin.setRange(5, 120)
        self.buffer_spin.setValue(30)
        buffer_layout.addWidget(self.buffer_spin)
        left_panel.addLayout(buffer_layout)
        
        # Control buttons
        self.start_btn = QPushButton("Stop Recording Buffer")
        self.start_btn.clicked.connect(self.toggle_recording)
        left_panel.addWidget(self.start_btn)
        
        self.save_btn = QPushButton("Save Clip (F9)")
        self.save_btn.clicked.connect(self.save_clip)
        left_panel.addWidget(self.save_btn)
        
        self.hotkey_btn = QPushButton("Hotkey: Enabled (F9)")
        self.hotkey_btn.clicked.connect(self.toggle_hotkey)
        left_panel.addWidget(self.hotkey_btn)
        
        # Clips list
        left_panel.addWidget(QLabel("Saved Clips:"))
        self.clips_list = QListWidget()
        self.clips_list.itemClicked.connect(self.on_clip_selected)
        left_panel.addWidget(self.clips_list)
        
        # Clip action buttons
        clip_actions = QHBoxLayout()
        
        self.trim_btn = QPushButton("Trim")
        self.trim_btn.clicked.connect(self.trim_clip)
        clip_actions.addWidget(self.trim_btn)
        
        self.rename_btn = QPushButton("Rename")
        self.rename_btn.clicked.connect(self.rename_clip)
        clip_actions.addWidget(self.rename_btn)
        
        self.delete_btn = QPushButton("Delete")
        self.delete_btn.clicked.connect(self.delete_clip)
        clip_actions.addWidget(self.delete_btn)
        
        self.upload_btn = QPushButton("Upload Clip")
        self.upload_btn.clicked.connect(self.upload_clip)
        clip_actions.addWidget(self.upload_btn)
        
        left_panel.addLayout(clip_actions)
        
        # Open folder button
        self.folder_btn = QPushButton("Open Clips Folder")
        self.folder_btn.clicked.connect(self.open_clips_folder)
        left_panel.addWidget(self.folder_btn)
        
        main_layout.addLayout(left_panel, 1)
        
        # Right panel - Video viewer
        self.viewer = ClipViewer()
        main_layout.addWidget(self.viewer, 2)
        
        central.setLayout(main_layout)
    
    def setup_signals(self):
        """Connect recorder signals"""
        self.recorder.signals.clip_saved.connect(self.on_clip_saved)
        self.recorder.signals.status_update.connect(self.update_status)
        self.recorder.signals.error_occurred.connect(self.show_error)
        self.recorder.signals.permission_needed.connect(self.show_permission_dialog)
    
    def show_permission_dialog(self):
        """Show macOS permission instructions"""
        msg = QMessageBox(self)
        msg.setWindowTitle("Screen Recording Permission Required")
        msg.setIcon(QMessageBox.Icon.Information)
        msg.setText(
            "<b>Screen Recording Permission Needed</b><br><br>"
            "This app needs permission to record your screen.<br><br>"
            "<b>To grant permission:</b><br>"
            "1. Open <b>System Settings</b><br>"
            "2. Go to <b>Privacy & Security</b> → <b>Screen Recording</b><br>"
            "3. Enable permission for <b>Terminal</b> or <b>Python</b><br>"
            "4. Restart this application<br><br>"
            "The app will open System Settings for you when you click OK."
        )
        
        open_settings_btn = msg.addButton("Open System Settings", QMessageBox.ButtonRole.AcceptRole)
        msg.addButton("Later", QMessageBox.ButtonRole.RejectRole)
        
        result = msg.exec()
        
        if msg.clickedButton() == open_settings_btn:
            import subprocess
            # Open System Settings to Privacy & Security > Screen Recording
            subprocess.run([
                "open",
                "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"
            ])
        
        self.update_status("Waiting for screen recording permission...")
        self.start_btn.setText("Start Recording Buffer")
    
    def auto_start_recording(self):
        """Auto-start recording on launch"""
        if not self.recorder.is_recording:
            success = self.recorder.start_recording()
            if success:
                self.update_status("Recording buffer active (auto-started)")
    
    def upload_clip(self):
    """Upload selected clip to cloud server"""
    current_item = self.clips_list.currentItem()
    if not current_item:
        QMessageBox.information(self, "No Clip Selected", 
                            "Please select a clip to upload.")
        return
    
    filename = current_item.data(Qt.ItemDataRole.UserRole)
    clip_path = self.recorder.clips_dir / filename
    
    if not clip_path.exists():
        self.show_error("Clip file does not exist.")
        return
    
    # Check file size
    file_size = clip_path.stat().st_size
    file_size_mb = file_size / (1024 * 1024)
    
    # Get server URL from settings or use default
    if not hasattr(self, 'server_url') or not self.server_url:
        # Prompt for server URL on first upload
        self.server_url = self.prompt_server_url()
    
    if not self.server_url:
        return  # User cancelled
    
    # Check file size limit
    if file_size_mb > 50:
        self.show_error(
            f"File too large ({file_size_mb:.1f}MB)\n\n"
            "The server has a 50MB limit.\n"
            "Please trim your clip to reduce file size."
        )
        return
    
    try:
        self.update_status(f"Uploading clip ({file_size_mb:.1f}MB)...")
        QApplication.processEvents()
        
        with open(clip_path, 'rb') as f:
            files = {'file': (filename, f, 'video/mp4')}
            
            response = requests.post(
                f'{self.server_url}/api/upload',
                files=files,
                timeout=300
            )
            
            if response.status_code == 200:
                result = response.json()
                
                if result.get('success'):
                    clip_url = result['url']
                    direct_url = result['direct_url']
                    
                    # Copy URL to clipboard
                    QApplication.clipboard().setText(clip_url)
                    
                    self.update_status(f"Upload successful!")
                    self.show_upload_success(clip_url, direct_url, filename)
                else:
                    self.show_error(f"Upload failed: {result.get('error', 'Unknown error')}")
            else:
                error_msg = "Unknown error"
                try:
                    error_data = response.json()
                    error_msg = error_data.get('error', error_msg)
                except:
                    pass
                
                self.show_error(
                    f"Upload failed: {error_msg}\n\n"
                    f"Status code: {response.status_code}"
                )
                
    except requests.exceptions.ConnectionError:
        self.show_error(
            f"Could not connect to server\n\n"
            f"URL: {self.server_url}\n\n"
            "Please check:\n"
            "• Is the URL correct?\n"
            "• Is the server running?\n"
            "• Is your internet working?"
        )
        # Clear saved URL so user can try a different one
        self.server_url = None
        
    except requests.exceptions.Timeout:
        self.show_error(
            "Upload timed out\n\n"
            "This could be because:\n"
            "• The file is very large\n"
            "• Your internet is slow\n"
            "• The server is not responding\n\n"
            "Try trimming the clip to reduce file size."
        )
    except Exception as e:
        self.show_error(f"Upload error: {str(e)}")

def prompt_server_url(self):
    """Prompt user for server URL"""
    from PyQt6.QtWidgets import QInputDialog
    
    # Provide helpful default examples
    default_url = "https://your-app.railway.app"
    
    dialog = QInputDialog(self)
    dialog.setWindowTitle("Server URL")
    dialog.setLabelText(
        "Enter your Screen Clips server URL:\n\n"
        "Examples:\n"
        "• https://your-app.railway.app\n"
        "• https://your-app.onrender.com\n"
        "• https://your-app.fly.dev\n"
        "• http://localhost:5000 (local testing)"
    )
    dialog.setTextValue(default_url)
    dialog.resize(500, 200)
    
    if dialog.exec() == QInputDialog.DialogCode.Accepted:
        url = dialog.textValue().strip().rstrip('/')
        if url:
            # Validate URL format
            if not url.startswith('http://') and not url.startswith('https://'):
                self.show_error("URL must start with http:// or https://")
                return self.prompt_server_url()  # Try again
            return url
    
    return None

def show_upload_success(self, clip_url, direct_url, filename):
    """Show upload success dialog"""
    msg = QMessageBox(self)
    msg.setWindowTitle("Upload Successful!")
    msg.setIcon(QMessageBox.Icon.Information)
    
    message = f"""
<b>✓ Clip uploaded successfully!</b><br><br>
<b>File:</b> {filename}<br>
<b>Watch URL:</b> <a href="{clip_url}">{clip_url}</a><br><br>
<i>The URL has been copied to your clipboard.</i><br><br>
Share this link with anyone to let them watch your clip!
    """
    
    msg.setText(message)
    msg.setTextFormat(Qt.TextFormat.RichText)
    
    open_btn = msg.addButton("Open in Browser", QMessageBox.ButtonRole.AcceptRole)
    copy_btn = msg.addButton("Copy URL Again", QMessageBox.ButtonRole.ActionRole)
    ok_btn = msg.addButton("OK", QMessageBox.ButtonRole.RejectRole)
    
    msg.exec()
    
    clicked = msg.clickedButton()
    if clicked == open_btn:
        import webbrowser
        webbrowser.open(clip_url)
    elif clicked == copy_btn:
        QApplication.clipboard().setText(clip_url)
        self.update_status("URL copied to clipboard")
    
    def auto_enable_hotkey(self):
        """Auto-enable hotkey on launch"""
        if not self.hotkey_registered:
            try:
                keyboard.add_hotkey('f9', self.save_clip)
                self.hotkey_registered = True
                self.update_status("Ready! Press F9 to save clips")
            except Exception as e:
                self.show_error(f"Failed to register hotkey: {str(e)}")
    
    def toggle_recording(self):
        """Start/stop recording buffer"""
        if not self.recorder.is_recording:
            self.recorder.buffer_seconds = self.buffer_spin.value()
            self.recorder.frame_buffer = deque(maxlen=self.recorder.buffer_seconds * self.recorder.fps)
            self.recorder.start_recording()
            self.start_btn.setText("Stop Recording Buffer")
            self.save_btn.setEnabled(True)
        else:
            self.recorder.stop_recording()
            self.start_btn.setText("Start Recording Buffer")
            self.save_btn.setEnabled(False)
    
    def save_clip(self):
        """Save clip from buffer"""
        filename = self.recorder.save_clip()
        if filename:
            self.update_status("Saving clip...")
    
    def toggle_hotkey(self):
        """Enable/disable F9 hotkey"""
        if not self.hotkey_registered:
            try:
                keyboard.add_hotkey('f9', self.save_clip)
                self.hotkey_registered = True
                self.hotkey_btn.setText("Hotkey: Enabled (F9)")
                self.update_status("Hotkey F9 enabled")
            except Exception as e:
                self.show_error(f"Failed to register hotkey: {str(e)}")
        else:
            try:
                keyboard.remove_hotkey('f9')
                self.hotkey_registered = False
                self.hotkey_btn.setText("Hotkey: Disabled")
                self.update_status("Hotkey F9 disabled")
            except:
                pass
    
    def rename_clip(self):
        """Rename selected clip"""
        current_item = self.clips_list.currentItem()
        if not current_item:
            QMessageBox.information(self, "No Clip Selected", 
                                   "Please select a clip to rename.")
            return
        
        old_filename = current_item.data(Qt.ItemDataRole.UserRole)
        old_path = self.recorder.clips_dir / old_filename
        meta_file = old_path.with_suffix('.meta')
        
        current_custom_name = ""
        if meta_file.exists():
            try:
                with open(meta_file, 'r') as f:
                    lines = f.readlines()
                    for line in lines:
                        if line.startswith('name='):
                            current_custom_name = line.split('=', 1)[1].strip()
                            break
            except:
                pass
        
        new_name, ok = QInputDialog.getText(
            self, 
            "Rename Clip",
            "Enter new name for clip:",
            QLineEdit.EchoMode.Normal,
            current_custom_name
        )
        
        if ok:
            new_name = new_name.strip()
            
            try:
                timestamp = ""
                if meta_file.exists():
                    with open(meta_file, 'r') as f:
                        lines = f.readlines()
                        for line in lines:
                            if line.startswith('timestamp='):
                                timestamp = line.split('=', 1)[1].strip()
                                break
                
                with open(meta_file, 'w') as f:
                    f.write(f"timestamp={timestamp}\n")
                    f.write(f"name={new_name}\n")
                
                self.load_clips_list()
                
                for i in range(self.clips_list.count()):
                    if self.clips_list.item(i).data(Qt.ItemDataRole.UserRole) == old_filename:
                        self.clips_list.setCurrentRow(i)
                        break
                
                if new_name:
                    self.update_status(f"Renamed to: {new_name}")
                else:
                    self.update_status("Clip name cleared")
                
            except Exception as e:
                self.show_error(f"Failed to rename clip: {str(e)}")
    
    def trim_clip(self):
        """Open trim dialog for selected clip"""
        current_item = self.clips_list.currentItem()
        if not current_item:
            QMessageBox.information(self, "No Clip Selected", 
                                   "Please select a clip to trim.")
            return
        
        filename = current_item.data(Qt.ItemDataRole.UserRole)
        clip_path = self.recorder.clips_dir / filename
        
        if self.viewer.current_clip_path == str(clip_path):
            self.viewer.release_current_clip()
        
        dialog = TrimDialog(str(clip_path), self)
        
        if dialog.exec() == QDialog.DialogCode.Accepted:
            start_frame, end_frame = dialog.get_trim_range()
            self.create_trimmed_video(str(clip_path), start_frame, end_frame)
    
    def create_trimmed_video(self, source_path, start_frame, end_frame):
        """Create a new trimmed video"""
        try:
            self.update_status("Creating trimmed clip...")
            
            cap = cv2.VideoCapture(source_path)
            fps = cap.get(cv2.CAP_PROP_FPS)
            width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
            height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
            
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            readable_time = datetime.now().strftime("%Y-%m-%d %I:%M:%S %p")
            output_path = self.recorder.clips_dir / f"clip_{timestamp}_trimmed.mp4"
            meta_path = output_path.with_suffix('.meta')
            
            fourcc = cv2.VideoWriter_fourcc(*'mp4v')
            out = cv2.VideoWriter(str(output_path), fourcc, fps, (width, height))
            
            cap.set(cv2.CAP_PROP_POS_FRAMES, start_frame)
            
            for frame_num in range(start_frame, end_frame + 1):
                ret, frame = cap.read()
                if not ret:
                    break
                out.write(frame)
            
            cap.release()
            out.release()
            
            with open(meta_path, 'w') as f:
                f.write(f"timestamp={readable_time}\n")
                f.write(f"name=(Trimmed)\n")
            
            self.update_status(f"Trimmed clip saved: {output_path.name}")
            self.load_clips_list()
            
            for i in range(self.clips_list.count()):
                item = self.clips_list.item(i)
                if item.data(Qt.ItemDataRole.UserRole) == output_path.name:
                    self.clips_list.setCurrentRow(i)
                    self.viewer.load_clip(str(output_path))
                    break
            
        except Exception as e:
            self.show_error(f"Failed to trim clip: {str(e)}")
    
    def load_clips_list(self):
        """Load list of saved clips with custom names"""
        self.clips_list.clear()
        clips = sorted(self.recorder.clips_dir.glob("*.mp4"), reverse=True)
        
        for clip in clips:
            meta_file = clip.with_suffix('.meta')
            display_name = clip.stem
            
            if meta_file.exists():
                try:
                    with open(meta_file, 'r') as f:
                        lines = f.readlines()
                        metadata = {}
                        for line in lines:
                            if '=' in line:
                                key, value = line.strip().split('=', 1)
                                metadata[key] = value
                        
                        custom_name = metadata.get('name', '').strip()
                        timestamp = metadata.get('timestamp', '').strip()
                        
                        if custom_name:
                            display_name = f"{custom_name} ({timestamp})"
                        elif timestamp:
                            display_name = f"Clip - {timestamp}"
                except:
                    pass
            
            from PyQt6.QtWidgets import QListWidgetItem
            item = QListWidgetItem(display_name)
            item.setData(Qt.ItemDataRole.UserRole, clip.name)
            self.clips_list.addItem(item)
    
    def on_clip_selected(self, item):
        """Handle clip selection"""
        filename = item.data(Qt.ItemDataRole.UserRole)
        clip_path = self.recorder.clips_dir / filename
        self.viewer.load_clip(str(clip_path))
    
    def on_clip_saved(self, filename):
        """Handle new clip saved"""
        self.load_clips_list()
    
    def delete_clip(self):
        """Delete selected clip"""
        current_item = self.clips_list.currentItem()
        if not current_item:
            return
        
        filename = current_item.data(Qt.ItemDataRole.UserRole)
        display_name = current_item.text()
        
        reply = QMessageBox.question(self, 'Delete Clip',
                                    f'Delete "{display_name}"?',
                                    QMessageBox.StandardButton.Yes | 
                                    QMessageBox.StandardButton.No)
        
        if reply == QMessageBox.StandardButton.Yes:
            clip_path = self.recorder.clips_dir / filename
            meta_path = clip_path.with_suffix('.meta')
            
            if self.viewer.current_clip_path == str(clip_path):
                self.viewer.release_current_clip()
            
            max_attempts = 5
            for attempt in range(max_attempts):
                try:
                    import gc
                    gc.collect()
                    
                    import time
                    time.sleep(0.2 * (attempt + 1))
                    
                    if clip_path.exists():
                        clip_path.unlink()
                    
                    if meta_path.exists():
                        meta_path.unlink()
                    
                    self.load_clips_list()
                    self.update_status(f"Deleted {display_name}")
                    return
                    
                except PermissionError as e:
                    if attempt < max_attempts - 1:
                        continue
                    else:
                        self.show_error(f"Failed to delete after {max_attempts} attempts.\n\n"
                                      f"The file is still being used by another process.\n"
                                      f"Please close any video players or file explorers viewing this file.\n\n"
                                      f"Error: {str(e)}")
                except Exception as e:
                    self.show_error(f"Failed to delete: {str(e)}")
                    return
    
    def open_clips_folder(self):
        """Open clips folder in file explorer"""
        import subprocess
        import platform
        
        folder = str(self.recorder.clips_dir)
        
        if platform.system() == "Windows":
            subprocess.run(["explorer", folder])
        elif platform.system() == "Darwin":
            subprocess.run(["open", folder])
        else:
            subprocess.run(["xdg-open", folder])
    
    def update_status(self, message):
        """Update status label"""
        self.status_label.setText(f"Status: {message}")
    
    def show_error(self, message):
        """Show error message"""
        QMessageBox.critical(self, "Error", message)
    
    def closeEvent(self, event):
        """Clean up on close"""
        self.viewer.release_current_clip()
        self.recorder.stop_recording()
        
        if self.hotkey_registered:
            try:
                keyboard.unhook_all()
            except:
                pass
        
        event.accept()

def main():
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec())

if __name__ == "__main__":
    main()

