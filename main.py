import sys
import cv2
import numpy as np
import threading
import time
from datetime import datetime
from collections import deque
from pathlib import Path
import platform
import subprocess
import tempfile
import os
from PyQt6.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout, 
                             QHBoxLayout, QPushButton, QLabel, QListWidget, 
                             QMessageBox, QFileDialog, QSlider, QSpinBox,
                             QDialog, QDialogButtonBox, QLineEdit, QInputDialog,
                             QComboBox)
from PyQt6.QtCore import QTimer, Qt, pyqtSignal, QObject
from PyQt6.QtGui import QPixmap, QImage
import keyboard
import requests

# Cross-platform screen capture
try:
    import mss
    HAS_MSS = True
except:
    HAS_MSS = False

# Audio recording
try:
    import sounddevice as sd
    import soundfile as sf
    HAS_AUDIO = True
except:
    HAS_AUDIO = False


class RecorderSignals(QObject):
    """Signals for thread-safe communication"""
    clip_saved = pyqtSignal(str)
    status_update = pyqtSignal(str)
    error_occurred = pyqtSignal(str)
    upload_progress = pyqtSignal(str)


class ScreenRecorder:
    """Handles continuous screen recording and clip saving with audio"""
    
    def __init__(self, buffer_seconds=30, fps=30):
        self.buffer_seconds = buffer_seconds
        self.fps = fps
        self.frame_buffer = deque(maxlen=buffer_seconds * fps)
        self.audio_buffer = deque(maxlen=buffer_seconds * 2)  # Store 0.5s chunks
        self.is_recording = False
        self.record_thread = None
        self.audio_thread = None
        self.signals = RecorderSignals()
        self.clips_dir = Path.home() / "ScreenClips"
        self.clips_dir.mkdir(exist_ok=True)
        
        # Audio settings
        self.sample_rate = 44100
        self.audio_chunk_duration = 0.5  # seconds
        
        # Backend URL
        self.backend_url = "https://clipperwebserviceapi-1.onrender.com"
        
    def start_recording(self):
        """Start continuous buffer recording"""
        if not self.is_recording:
            self.is_recording = True
            self.record_thread = threading.Thread(target=self._record_loop, daemon=True)
            self.record_thread.start()
            
            if HAS_AUDIO:
                self.audio_thread = threading.Thread(target=self._audio_loop, daemon=True)
                self.audio_thread.start()
                self.signals.status_update.emit("Recording video + audio")
            else:
                self.signals.status_update.emit("Recording video only (install sounddevice for audio)")
    
    def stop_recording(self):
        """Stop buffer recording"""
        self.is_recording = False
        if self.record_thread:
            self.record_thread.join(timeout=2)
        if self.audio_thread:
            self.audio_thread.join(timeout=2)
        self.signals.status_update.emit("Recording stopped")
    
    def _record_loop(self):
        """Continuously capture screen to buffer"""
        if not HAS_MSS:
            self.signals.error_occurred.emit("mss not installed. Run: pip install mss")
            self.is_recording = False
            return
            
        with mss.mss() as sct:
            monitor = sct.monitors[1]  # Primary monitor
            frame_delay = 1.0 / self.fps
            
            while self.is_recording:
                start_time = time.time()
                
                try:
                    # Capture screen
                    screenshot = sct.grab(monitor)
                    frame = np.array(screenshot)
                    
                    # Convert BGRA to BGR
                    frame = cv2.cvtColor(frame, cv2.COLOR_BGRA2BGR)
                    
                    # Add timestamp
                    timestamp = datetime.now()
                    self.frame_buffer.append((frame, timestamp))
                    
                except Exception as e:
                    print(f"Frame capture error: {e}")
                
                # Maintain target FPS
                elapsed = time.time() - start_time
                sleep_time = max(0, frame_delay - elapsed)
                time.sleep(sleep_time)
    
    def _audio_loop(self):
        """Continuously capture audio to buffer"""
        chunk_samples = int(self.sample_rate * self.audio_chunk_duration)
        
        def audio_callback(indata, frames, time_info, status):
            if status:
                print(f"Audio status: {status}")
            # Store audio chunk
            self.audio_buffer.append(indata.copy())
        
        try:
            with sd.InputStream(samplerate=self.sample_rate, 
                              channels=2,
                              callback=audio_callback,
                              blocksize=chunk_samples):
                while self.is_recording:
                    time.sleep(0.1)
        except Exception as e:
            print(f"Audio recording error: {e}")
    
    def save_clip(self, duration_seconds=None):
        """Save the last N seconds from buffer with audio"""
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
        
        # Get corresponding audio chunks
        audio_chunks = list(self.audio_buffer) if HAS_AUDIO else []
        
        # Generate filename with metadata
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        readable_time = datetime.now().strftime("%Y-%m-%d %I:%M:%S %p")
        filename = self.clips_dir / f"clip_{timestamp}.mp4"
        metadata_file = self.clips_dir / f"clip_{timestamp}.meta"
        
        # Save video in background thread
        threading.Thread(target=self._save_video, 
                        args=(frames_list, audio_chunks, filename, metadata_file, readable_time), 
                        daemon=True).start()
        
        return str(filename)
    
    def _save_video(self, frames_list, audio_chunks, filename, metadata_file, readable_time):
        """Save frames and audio to video file using ffmpeg"""
        try:
            if not frames_list:
                return
            
            # Get frame dimensions
            height, width = frames_list[0][0].shape[:2]
            
            # For cross-platform compatibility, use ffmpeg if available
            # Otherwise fall back to basic OpenCV
            if self._has_ffmpeg() and HAS_AUDIO and audio_chunks:
                self._save_with_ffmpeg(frames_list, audio_chunks, filename, width, height)
            else:
                # Fallback: save video only with OpenCV
                self._save_with_opencv(frames_list, filename, width, height)
            
            # Save metadata file with timestamp and custom name
            with open(metadata_file, 'w') as f:
                f.write(f"timestamp={readable_time}\n")
                f.write(f"name=\n")  # Empty name initially
            
            self.signals.clip_saved.emit(str(filename))
            self.signals.status_update.emit(f"Clip saved: {filename.name}")
            
        except Exception as e:
            self.signals.error_occurred.emit(f"Error saving clip: {str(e)}")
    
    def _has_ffmpeg(self):
        """Check if ffmpeg is available"""
        try:
            subprocess.run(['ffmpeg', '-version'], 
                         stdout=subprocess.DEVNULL, 
                         stderr=subprocess.DEVNULL)
            return True
        except:
            return False
    
    def _save_with_ffmpeg(self, frames_list, audio_chunks, filename, width, height):
        """Save video with audio using ffmpeg"""
        temp_video = tempfile.NamedTemporaryFile(suffix='.mp4', delete=False)
        temp_audio = tempfile.NamedTemporaryFile(suffix='.wav', delete=False)
        
        try:
            # Save video frames
            fourcc = cv2.VideoWriter_fourcc(*'mp4v')
            out = cv2.VideoWriter(temp_video.name, fourcc, self.fps, (width, height))
            for frame, _ in frames_list:
                out.write(frame)
            out.release()
            
            # Save audio
            if audio_chunks:
                audio_data = np.concatenate(audio_chunks, axis=0)
                sf.write(temp_audio.name, audio_data, self.sample_rate)
            
            temp_video.close()
            temp_audio.close()
            
            # Combine with ffmpeg
            cmd = [
                'ffmpeg', '-y',
                '-i', temp_video.name,
                '-i', temp_audio.name,
                '-c:v', 'libx264',
                '-c:a', 'aac',
                '-shortest',
                str(filename)
            ]
            subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            
        finally:
            # Cleanup temp files
            try:
                os.unlink(temp_video.name)
                os.unlink(temp_audio.name)
            except:
                pass
    
    def _save_with_opencv(self, frames_list, filename, width, height):
        """Fallback: save video only with OpenCV"""
        fourcc = cv2.VideoWriter_fourcc(*'mp4v')
        out = cv2.VideoWriter(str(filename), fourcc, self.fps, (width, height))
        
        for frame, _ in frames_list:
            out.write(frame)
        
        out.release()
    
    def upload_clip(self, filepath, username="Anonymous"):
        """Upload clip to backend server with username"""
        try:
            self.signals.upload_progress.emit("Uploading to server...")
            
            with open(filepath, 'rb') as f:
                files = {'file': (Path(filepath).name, f, 'video/mp4')}
                data = {'username': username}
                response = requests.post(
                    f"{self.backend_url}/upload",
                    files=files,
                    data=data,
                    timeout=120
                )
            
            if response.status_code == 200:
                data = response.json()
                url = data.get('url', '')
                clip_id = data.get('clip_id', '')
                
                self.signals.upload_progress.emit(f"Upload complete! ID: {clip_id}")
                return True, url
            else:
                self.signals.upload_progress.emit(f"Upload failed: {response.status_code}")
                return False, None
                
        except Exception as e:
            self.signals.upload_progress.emit(f"Upload error: {str(e)}")
            return False, None


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
        self.show_frame(0)
    
    def on_start_changed(self, value):
        if value >= self.end_frame:
            value = self.end_frame - 1
            self.start_slider.setValue(value)
        self.start_frame = value
        self.start_time_label.setText(f"{value / self.fps:.1f}s")
        self.update_time_label()
        self.show_frame(value)
    
    def on_end_changed(self, value):
        if value <= self.start_frame:
            value = self.start_frame + 1
            self.end_slider.setValue(value)
        self.end_frame = value
        self.end_time_label.setText(f"{value / self.fps:.1f}s")
        self.update_time_label()
        self.show_frame(value)
    
    def update_time_label(self):
        duration = (self.end_frame - self.start_frame) / self.fps
        self.time_label.setText(f"Trimmed Duration: {duration:.1f}s")
    
    def show_frame(self, frame_num):
        self.cap.set(cv2.CAP_PROP_POS_FRAMES, frame_num)
        ret, frame = self.cap.read()
        if ret:
            rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            h, w, ch = rgb_frame.shape
            bytes_per_line = ch * w
            q_img = QImage(rgb_frame.data, w, h, bytes_per_line, QImage.Format.Format_RGB888)
            pixmap = QPixmap.fromImage(q_img)
            scaled_pixmap = pixmap.scaled(self.preview_label.size(), 
                                          Qt.AspectRatioMode.KeepAspectRatio,
                                          Qt.TransformationMode.SmoothTransformation)
            self.preview_label.setPixmap(scaled_pixmap)
    
    def get_trim_range(self):
        return self.start_frame, self.end_frame
    
    def closeEvent(self, event):
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
        
        self.video_label = QLabel("No clip loaded")
        self.video_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.video_label.setMinimumSize(640, 480)
        self.video_label.setStyleSheet("background-color: black; color: white;")
        layout.addWidget(self.video_label)
        
        self.time_label = QLabel("0:00 / 0:00")
        self.time_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self.time_label)
        
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
        
        self.timer = QTimer()
        self.timer.timeout.connect(self.update_frame)
        self.is_playing = False
        
        self.setLayout(layout)
    
    def release_current_clip(self):
        if self.is_playing:
            self.is_playing = False
            self.timer.stop()
            self.play_btn.setText("Play")
        
        if self.cap:
            self.cap.release()
            self.cap = None
        
        import gc
        gc.collect()
        
        self.video_label.clear()
        self.video_label.setText("No clip loaded")
        self.time_label.setText("0:00 / 0:00")
        self.position_slider.setValue(0)
        self.play_btn.setEnabled(False)
        self.position_slider.setEnabled(False)
        
        self.current_clip_path = None
    
    def load_clip(self, filepath):
        self.release_current_clip()
        
        self.current_clip_path = filepath
        self.cap = cv2.VideoCapture(filepath)
        
        if self.cap.isOpened():
            self.play_btn.setEnabled(True)
            self.position_slider.setEnabled(True)
            
            total_frames = int(self.cap.get(cv2.CAP_PROP_FRAME_COUNT))
            self.position_slider.setMaximum(total_frames - 1)
            
            self.show_frame(0)
            self.update_time_display()
        
    def toggle_play(self):
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
        if not self.cap:
            return
        
        ret, frame = self.cap.read()
        
        if ret:
            self.display_frame(frame)
            current_pos = int(self.cap.get(cv2.CAP_PROP_POS_FRAMES))
            self.position_slider.setValue(current_pos)
            self.update_time_display()
        else:
            self.is_playing = False
            self.play_btn.setText("Play")
            self.timer.stop()
            self.cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
            self.position_slider.setValue(0)
    
    def show_frame(self, frame_num):
        if not self.cap:
            return
        
        self.cap.set(cv2.CAP_PROP_POS_FRAMES, frame_num)
        ret, frame = self.cap.read()
        
        if ret:
            self.display_frame(frame)
    
    def seek(self, position):
        self.show_frame(position)
        self.update_time_display()
    
    def display_frame(self, frame):
        rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        h, w, ch = rgb_frame.shape
        bytes_per_line = ch * w
        q_img = QImage(rgb_frame.data, w, h, bytes_per_line, QImage.Format.Format_RGB888)
        pixmap = QPixmap.fromImage(q_img)
        scaled_pixmap = pixmap.scaled(self.video_label.size(), 
                                      Qt.AspectRatioMode.KeepAspectRatio,
                                      Qt.TransformationMode.SmoothTransformation)
        self.video_label.setPixmap(scaled_pixmap)
    
    def update_time_display(self):
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
        self.username = self.load_username()
        if not self.username:
            QTimer.singleShot(0, self.prompt_for_username)
        self.init_ui()
        self.setup_signals()
        self.load_clips_list()
        
        QTimer.singleShot(500, self.auto_start_recording)
        QTimer.singleShot(1000, self.auto_enable_hotkey)
    
    def load_username(self):
        username_file = Path.home() / "ScreenClips" / ".username"
        if username_file.exists():
            try:
                name = username_file.read_text().strip()
                if name:
                    return name
            except:
                pass
        return None
    
    def prompt_for_username(self):
        username, ok = QInputDialog.getText(
            self,
            "Choose a username",
            "Enter a username (shown when you upload clips):",
            QLineEdit.EchoMode.Normal
        )

        if ok:
            username = username.strip() or "Anonymous"
            self.username = username
            self.save_username(username)
            self.update_status(f"Username set to: {username}")
        else:
            self.username = "Anonymous"
            self.save_username("Anonymous")
    def save_username(self, username):
        """Save username to file"""
        username_file = Path.home() / "ScreenClips" / ".username"
        username_file.parent.mkdir(exist_ok=True)
        username_file.write_text(username)
        
    def init_ui(self):
        self.setWindowTitle("Screen Clip Recorder - Cross Platform")
        self.setGeometry(100, 100, 1000, 700)
        
        central = QWidget()
        self.setCentralWidget(central)
        
        main_layout = QHBoxLayout()
        
        # Left panel
        left_panel = QVBoxLayout()
        
        self.status_label = QLabel("Status: Starting...")
        left_panel.addWidget(self.status_label)
        
        # Buffer duration
        buffer_layout = QHBoxLayout()
        buffer_layout.addWidget(QLabel("Buffer (seconds):"))
        self.buffer_spin = QSpinBox()
        self.buffer_spin.setRange(5, 120)
        self.buffer_spin.setValue(30)
        buffer_layout.addWidget(self.buffer_spin)
        left_panel.addLayout(buffer_layout)
        
        # Control buttons
        self.start_btn = QPushButton("⏹️ Stop Recording")
        self.start_btn.clicked.connect(self.toggle_recording)
        left_panel.addWidget(self.start_btn)
        
        self.save_btn = QPushButton("💾 Save Clip (F9)")
        self.save_btn.clicked.connect(self.save_clip)
        left_panel.addWidget(self.save_btn)
        
        self.hotkey_btn = QPushButton("⌨️ Hotkey: Enabled (F9)")
        self.hotkey_btn.clicked.connect(self.toggle_hotkey)
        left_panel.addWidget(self.hotkey_btn)

        self.upload_btn = QPushButton("☁️ Upload Selected Clip")
        self.upload_btn.clicked.connect(self.on_upload_clicked)
        self.upload_btn.setEnabled(True)
        left_panel.addWidget(self.upload_btn)
        
        # Clips list
        left_panel.addWidget(QLabel("Saved Clips:"))
        self.clips_list = QListWidget()
        self.clips_list.itemClicked.connect(self.on_clip_selected)
        left_panel.addWidget(self.clips_list)
        
        # Clip actions
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
        
        left_panel.addLayout(clip_actions)
        
        self.folder_btn = QPushButton("Open Clips Folder")
        self.folder_btn.clicked.connect(self.open_clips_folder)
        left_panel.addWidget(self.folder_btn)
        
        main_layout.addLayout(left_panel, 1)
        
        # Right panel
        self.viewer = ClipViewer()
        main_layout.addWidget(self.viewer, 2)
        
        central.setLayout(main_layout)
    
    def setup_signals(self):
        self.recorder.signals.clip_saved.connect(self.on_clip_saved)
        self.recorder.signals.status_update.connect(self.update_status)
        self.recorder.signals.error_occurred.connect(self.show_error)
        self.recorder.signals.upload_progress.connect(self.update_status)
    
    def auto_start_recording(self):
        if not self.recorder.is_recording:
            self.recorder.start_recording()
    
    def auto_enable_hotkey(self):
        if not self.hotkey_registered:
            try:
                keyboard.add_hotkey('f9', self.save_clip)
                self.hotkey_registered = True
                self.update_status("Ready! Press F9 to save clips")
            except Exception as e:
                self.show_error(f"Failed to register hotkey: {str(e)}")
    
    def toggle_recording(self):
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
        filename = self.recorder.save_clip()
        if filename:
            self.update_status("Saving clip...")
    
    def toggle_hotkey(self):
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
    
    def on_set_username(self):
        """Save username when button clicked"""
        new_username = self.username_input.text().strip() or "Anonymous"
        self.username = new_username
        self.save_username(new_username)
        self.update_status(f"Username set to: {new_username}")
    
    def on_buffer_preset_changed(self, preset_text):
        """Handle buffer preset selection"""
        if preset_text == "Custom":
            self.custom_buffer_widget.setVisible(True)
        else:
            self.custom_buffer_widget.setVisible(False)
    
    def get_buffer_seconds(self):
        """Get buffer duration in seconds based on current selection"""
        preset = self.buffer_preset.currentText()
        
        if preset == "15 seconds":
            return 15
        elif preset == "30 seconds":
            return 30
        elif preset == "1 minute":
            return 60
        elif preset == "2 minutes":
            return 120
        elif preset == "5 minutes":
            return 300
        elif preset == "Custom":
            minutes = self.custom_minutes.value()
            seconds = self.custom_seconds.value()
            total = (minutes * 60) + seconds
            return max(5, total)  # Minimum 5 seconds
        
        return 30  # Default
    
    def on_upload_clicked(self):
        current_item = self.clips_list.currentItem()
        if not current_item:
            QMessageBox.information(self, "No Clip Selected", "Please select a clip to upload.")
            return

        filename = current_item.data(Qt.ItemDataRole.UserRole)
        clip_path = self.recorder.clips_dir / filename

        if not clip_path.exists():
            QMessageBox.information(self, "File Missing", "Selected clip not found on disk.")
            self.load_clips_list()
            return

        self.upload_btn.setEnabled(False)

        current_username = self.username or "Anonymous"

        threading.Threadq(
            target=self._do_upload,
            args=(str(clip_path), current_username),
            daemon=True
        ).start()
    
    def _do_upload(self, filepath, username):
        success, url = self.recorder.upload_clip(filepath, username)
        
        def show_result():
            self.upload_btn.setEnabled(True)
            if success and url:
                # Copy URL to clipboard
                QApplication.clipboard().setText(url)
                QMessageBox.information(self, "✅ Upload Successful", 
                                      f"Clip uploaded successfully!\n\n"
                                      f"View at:\n{url}\n\n"
                                      f"🔗 URL copied to clipboard!")
            else:
                QMessageBox.warning(self, "❌ Upload Failed", "Failed to upload clip to server.")
        
        QTimer.singleShot(0, show_result)
    
    def rename_clip(self):
        current_item = self.clips_list.currentItem()
        if not current_item:
            QMessageBox.information(self, "No Clip Selected", "Please select a clip to rename.")
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
        current_item = self.clips_list.currentItem()
        if not current_item:
            QMessageBox.information(self, "No Clip Selected", "Please select a clip to trim.")
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
        filename = item.data(Qt.ItemDataRole.UserRole)
        clip_path = self.recorder.clips_dir / filename
        self.viewer.load_clip(str(clip_path))
    
    def on_clip_saved(self, filename):
        self.load_clips_list()
    
    def delete_clip(self):
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
        import subprocess
        
        folder = str(self.recorder.clips_dir)
        
        if platform.system() == "Windows":
            subprocess.run(["explorer", folder])
        elif platform.system() == "Darwin":
            subprocess.run(["open", folder])
        else:
            subprocess.run(["xdg-open", folder])
    
    def update_status(self, message):
        QTimer.singleShot(0, lambda: self.status_label.setText(f"Status: {message}"))
    
    def show_error(self, message):
        def _show():
            QMessageBox.critical(self, "Error", message)
        QTimer.singleShot(0, _show)
    
    def closeEvent(self, event):
        self.viewer.release_current_clip()
        self.recorder.stop_recording()
        
        if self.hotkey_registered:
            try:
                keyboard.unhook_all()
            except:
                pass
        
        event.accept()


def main():
    # Check dependencies
    missing = []
    if not HAS_MSS:
        missing.append("mss")
    if not HAS_AUDIO:
        missing.append("sounddevice and soundfile")
    
    if missing:
        print(f"Warning: Missing dependencies: {', '.join(missing)}")
        print("Install with: pip install mss sounddevice soundfile")
        print("Continuing with limited functionality...\n")
    
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
