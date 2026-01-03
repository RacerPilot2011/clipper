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
                             QDialog, QDialogButtonBox, QLineEdit, QInputDialog,
                             QCheckBox, QComboBox, QGroupBox)
from PyQt6.QtCore import QTimer, Qt, pyqtSignal, QObject
from PyQt6.QtGui import QPixmap, QImage
import keyboard
import requests
import json
from urllib.parse import urlparse
import platform
import os
import tempfile
import subprocess

# Audio recording imports
try:
    import sounddevice as sd
    import soundfile as sf
    AUDIO_AVAILABLE = True
except ImportError:
    AUDIO_AVAILABLE = False
    print("Warning: sounddevice/soundfile not available. Audio recording disabled.")
    print("Install with: pip install sounddevice soundfile")

class RecorderSignals(QObject):
    """Signals for thread-safe communication"""
    clip_saved = pyqtSignal(str)
    status_update = pyqtSignal(str)
    error_occurred = pyqtSignal(str)
    permission_needed = pyqtSignal()
    audio_devices_found = pyqtSignal(list, list)

class AudioRecorder:
    """Handles audio recording from microphone and desktop"""
    
    def __init__(self, sample_rate=44100):
        self.sample_rate = sample_rate
        self.is_recording = False
        self.mic_enabled = False
        self.desktop_enabled = False
        self.mic_device = None
        self.desktop_device = None
        self.mic_buffer = deque()
        self.desktop_buffer = deque()
        self.record_thread = None
        self.lock = threading.Lock()
        
    def get_audio_devices(self):
        """Get available audio input devices"""
        if not AUDIO_AVAILABLE:
            return [], []
        
        try:
            devices = sd.query_devices()
            input_devices = []
            output_devices = []
            
            for i, device in enumerate(devices):
                if device['max_input_channels'] > 0:
                    input_devices.append({
                        'index': i,
                        'name': device['name'],
                        'channels': device['max_input_channels']
                    })
                if device['max_output_channels'] > 0:
                    output_devices.append({
                        'index': i,
                        'name': device['name'],
                        'channels': device['max_output_channels']
                    })
            
            return input_devices, output_devices
        except Exception as e:
            print(f"Error getting audio devices: {e}")
            return [], []
    
    def start_recording(self):
        """Start audio recording"""
        if not AUDIO_AVAILABLE:
            return False
        
        if not self.mic_enabled and not self.desktop_enabled:
            return False
        
        self.is_recording = True
        self.record_thread = threading.Thread(target=self._record_loop, daemon=True)
        self.record_thread.start()
        return True
    
    def stop_recording(self):
        """Stop audio recording"""
        self.is_recording = False
        if self.record_thread:
            self.record_thread.join(timeout=2)
    
    def _record_loop(self):
        """Audio recording loop"""
        try:
            # Determine which streams to open
            streams = []
            
            if self.mic_enabled and self.mic_device is not None:
                try:
                    mic_stream = sd.InputStream(
                        device=self.mic_device,
                        channels=1,
                        samplerate=self.sample_rate,
                        callback=lambda indata, frames, time, status: self._mic_callback(indata, frames, time, status)
                    )
                    streams.append(('mic', mic_stream))
                except Exception as e:
                    print(f"Failed to open mic stream: {e}")
            
            if self.desktop_enabled and self.desktop_device is not None:
                try:
                    # For desktop audio, we try to use loopback/stereo mix
                    desktop_stream = sd.InputStream(
                        device=self.desktop_device,
                        channels=2,
                        samplerate=self.sample_rate,
                        callback=lambda indata, frames, time, status: self._desktop_callback(indata, frames, time, status)
                    )
                    streams.append(('desktop', desktop_stream))
                except Exception as e:
                    print(f"Failed to open desktop stream: {e}")
            
            if not streams:
                return
            
            # Start all streams
            for name, stream in streams:
                stream.start()
            
            # Keep recording while active
            while self.is_recording:
                time.sleep(0.1)
            
            # Stop all streams
            for name, stream in streams:
                stream.stop()
                stream.close()
                
        except Exception as e:
            print(f"Audio recording error: {e}")
    
    def _mic_callback(self, indata, frames, time_info, status):
        """Callback for microphone audio"""
        with self.lock:
            self.mic_buffer.append(indata.copy())
    
    def _desktop_callback(self, indata, frames, time_info, status):
        """Callback for desktop audio"""
        with self.lock:
            self.desktop_buffer.append(indata.copy())
    
    def get_audio_data(self):
        """Get recorded audio data and clear buffers"""
        with self.lock:
            mic_data = list(self.mic_buffer)
            desktop_data = list(self.desktop_buffer)
            self.mic_buffer.clear()
            self.desktop_buffer.clear()
        
        return mic_data, desktop_data
    
    def clear_buffers(self):
        """Clear audio buffers"""
        with self.lock:
            self.mic_buffer.clear()
            self.desktop_buffer.clear()

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
        
        # Audio recorder
        self.audio_recorder = AudioRecorder() if AUDIO_AVAILABLE else None
        
    def check_screen_recording_permission(self):
        """Check if screen recording permission is granted"""
        if platform.system() != "Darwin":
            if platform.system() == "Linux":
                if not os.environ.get('DISPLAY'):
                    return False
            return True
        
        # For macOS, we need to check permissions properly
        # The permission check itself will trigger the prompt on first run
        # We cache the result to avoid repeated prompts
        if hasattr(self, '_mac_permission_checked') and self._mac_permission_checked:
            return self.permission_granted
        
        self._mac_permission_checked = True
        
        try:
            # Single permission check - don't create multiple mss instances
            if not hasattr(self, '_permission_test_done'):
                self._permission_test_done = True
                with mss.mss() as sct:
                    monitor = sct.monitors[1]
                    test_region = {
                        "top": monitor["top"],
                        "left": monitor["left"],
                        "width": 10,
                        "height": 10
                    }
                    screenshot = sct.grab(test_region)
                    # Check if we got actual pixel data
                    img = np.array(screenshot)
                    if img.size > 0 and not np.all(img == 0):
                        self.permission_granted = True
                        return True
                    else:
                        self.permission_granted = False
                        return False
            return self.permission_granted
        except Exception as e:
            print(f"Permission check error: {e}")
            self.permission_granted = False
            return False
    
    def start_recording(self):
        """Start continuous buffer recording"""
        # For macOS, only check permission once at startup
        if platform.system() == "Darwin":
            if not hasattr(self, '_initial_permission_check_done'):
                self._initial_permission_check_done = True
                if not self.check_screen_recording_permission():
                    self.signals.permission_needed.emit()
                    return False
                # Small delay to ensure permission dialog is handled
                time.sleep(0.5)
        
        if not self.is_recording:
            self.is_recording = True
            self.record_thread = threading.Thread(target=self._record_loop, daemon=True)
            self.record_thread.start()
            
            # Start audio recording if enabled
            if self.audio_recorder:
                self.audio_recorder.start_recording()
            
            self.signals.status_update.emit("Recording buffer active")
            return True
        return True
    
    def stop_recording(self):
        """Stop buffer recording"""
        self.is_recording = False
        if self.record_thread:
            self.record_thread.join(timeout=2)
        
        # Stop audio recording
        if self.audio_recorder:
            self.audio_recorder.stop_recording()
        
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
            # Reuse the same instance to avoid repeated permission prompts on macOS
            self.sct = mss.mss()
            
            monitor = self.sct.monitors[1]
            
            if platform.system() == "Linux":
                monitor = {
                    "top": 0,
                    "left": 0,
                    "width": monitor["width"],
                    "height": monitor["height"],
                    "mon": 1
                }
            
            frame_delay = 1.0 / self.fps
            consecutive_errors = 0
            max_consecutive_errors = 10
            
            while self.is_recording:
                start_time = time.time()
                
                try:
                    screenshot = self.sct.grab(monitor)
                    frame = np.array(screenshot)
                    
                    # macOS specific: Check if we got black/empty frames (permission issue)
                    if platform.system() == "Darwin":
                        if frame.size == 0 or np.all(frame == 0):
                            consecutive_errors += 1
                            if consecutive_errors >= max_consecutive_errors:
                                self.signals.error_occurred.emit(
                                    "Screen recording permission may have been denied.\n\n"
                                    "Please grant permission in System Settings:\n"
                                    "Privacy & Security → Screen Recording"
                                )
                                self.is_recording = False
                                break
                            time.sleep(0.5)
                            continue
                    
                    consecutive_errors = 0  # Reset on successful frame
                    
                    if frame.shape[2] == 4:
                        frame = cv2.cvtColor(frame, cv2.COLOR_BGRA2BGR)
                    elif frame.shape[2] == 3:
                        frame = cv2.cvtColor(frame, cv2.COLOR_RGB2BGR)
                    
                    timestamp = datetime.now()
                    self.frame_buffer.append((frame, timestamp))
                    
                except Exception as e:
                    error_msg = str(e)
                    consecutive_errors += 1
                    
                    if consecutive_errors >= max_consecutive_errors:
                        if "XGetImage" in error_msg or "X11" in error_msg:
                            self.signals.error_occurred.emit(
                                "X11 capture error. Try:\n"
                                "1. Run: xhost +local:\n"
                                "2. Or set DISPLAY variable\n"
                                "3. Check if running in Wayland (may need XWayland)"
                            )
                        elif platform.system() == "Darwin":
                            self.signals.error_occurred.emit(
                                "Screen recording failed.\n\n"
                                "Please:\n"
                                "1. Quit this application completely\n"
                                "2. Open System Settings → Privacy & Security → Screen Recording\n"
                                "3. Enable permission for Python/Terminal\n"
                                "4. Restart this application\n\n"
                                "Note: Do NOT run with sudo - it causes permission issues."
                            )
                        else:
                            self.signals.error_occurred.emit(f"Recording error: {error_msg}")
                        
                        self.is_recording = False
                        break
                    
                    time.sleep(0.5)
                    continue
                
                elapsed = time.time() - start_time
                sleep_time = max(0, frame_delay - elapsed)
                time.sleep(sleep_time)
            
            if self.sct:
                try:
                    self.sct.close()
                except:
                    pass
                self.sct = None
                    
        except Exception as e:
            self.signals.error_occurred.emit(f"Recording error: {str(e)}")
            self.is_recording = False
            
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
        
        frames_to_save = min(int(duration_seconds * self.fps), len(self.frame_buffer))
        
        if frames_to_save == 0:
            self.signals.error_occurred.emit("Not enough frames")
            return None
        
        frames_list = list(self.frame_buffer)[-frames_to_save:]
        
        # Get audio data if available
        mic_data, desktop_data = None, None
        if self.audio_recorder and AUDIO_AVAILABLE:
            mic_data, desktop_data = self.audio_recorder.get_audio_data()
        
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        readable_time = datetime.now().strftime("%Y-%m-%d %I:%M:%S %p")
        filename = self.clips_dir / f"clip_{timestamp}.mp4"
        metadata_file = self.clips_dir / f"clip_{timestamp}.meta"
        
        threading.Thread(target=self._save_video, 
                        args=(frames_list, filename, metadata_file, readable_time, mic_data, desktop_data), 
                        daemon=True).start()
        
        return str(filename)
    
    def _save_video(self, frames_list, filename, metadata_file, readable_time, mic_data, desktop_data):
        """Save frames to video file with audio"""
        try:
            if not frames_list:
                return
            
            height, width = frames_list[0][0].shape[:2]
            
            # Save temporary video without audio
            temp_video = tempfile.NamedTemporaryFile(suffix='.mp4', delete=False)
            temp_video_path = temp_video.name
            temp_video.close()
            
            fourcc = cv2.VideoWriter_fourcc(*'mp4v')
            out = cv2.VideoWriter(temp_video_path, fourcc, self.fps, (width, height))
            
            for frame, _ in frames_list:
                out.write(frame)
            
            out.release()
            
            # Process audio if available
            has_audio = False
            temp_audio_files = []
            
            if AUDIO_AVAILABLE and (mic_data or desktop_data):
                try:
                    # Save mic audio
                    if mic_data and len(mic_data) > 0:
                        mic_audio_path = tempfile.NamedTemporaryFile(suffix='.wav', delete=False).name
                        mic_audio = np.concatenate(mic_data, axis=0)
                        sf.write(mic_audio_path, mic_audio, self.audio_recorder.sample_rate)
                        temp_audio_files.append(('mic', mic_audio_path))
                    
                    # Save desktop audio
                    if desktop_data and len(desktop_data) > 0:
                        desktop_audio_path = tempfile.NamedTemporaryFile(suffix='.wav', delete=False).name
                        desktop_audio = np.concatenate(desktop_data, axis=0)
                        sf.write(desktop_audio_path, desktop_audio, self.audio_recorder.sample_rate)
                        temp_audio_files.append(('desktop', desktop_audio_path))
                    
                    if temp_audio_files:
                        has_audio = True
                except Exception as e:
                    print(f"Audio processing error: {e}")
            
            # Combine video and audio using ffmpeg
            if has_audio and self._check_ffmpeg():
                try:
                    self._merge_video_audio(temp_video_path, temp_audio_files, str(filename))
                except Exception as e:
                    print(f"FFmpeg merge error: {e}")
                    # Fall back to video only
                    import shutil
                    shutil.move(temp_video_path, str(filename))
            else:
                # No audio or ffmpeg not available, use video only
                import shutil
                shutil.move(temp_video_path, str(filename))
            
            # Clean up temporary files
            try:
                if os.path.exists(temp_video_path):
                    os.unlink(temp_video_path)
                for _, audio_path in temp_audio_files:
                    if os.path.exists(audio_path):
                        os.unlink(audio_path)
            except:
                pass
            
            # Save metadata
            with open(metadata_file, 'w') as f:
                f.write(f"timestamp={readable_time}\n")
                f.write(f"name=\n")
            
            self.signals.clip_saved.emit(str(filename))
            self.signals.status_update.emit(f"Clip saved: {filename.name}")
            
        except Exception as e:
            self.signals.error_occurred.emit(f"Error saving clip: {str(e)}")
    
    def _check_ffmpeg(self):
        """Check if ffmpeg is available"""
        try:
            subprocess.run(['ffmpeg', '-version'], 
                         stdout=subprocess.DEVNULL, 
                         stderr=subprocess.DEVNULL)
            return True
        except FileNotFoundError:
            return False
    
    def _merge_video_audio(self, video_path, audio_files, output_path):
        """Merge video with audio tracks using ffmpeg"""
        if not audio_files:
            import shutil
            shutil.move(video_path, output_path)
            return
        
        # Build ffmpeg command
        cmd = ['ffmpeg', '-y', '-i', video_path]
        
        # Add audio inputs
        for _, audio_path in audio_files:
            cmd.extend(['-i', audio_path])
        
        # Mix audio if multiple sources
        if len(audio_files) > 1:
            cmd.extend([
                '-filter_complex', 
                f'[1:a][2:a]amix=inputs={len(audio_files)}:duration=first[aout]',
                '-map', '0:v',
                '-map', '[aout]'
            ])
        else:
            cmd.extend(['-map', '0:v', '-map', '1:a'])
        
        # Output settings
        cmd.extend([
            '-c:v', 'copy',
            '-c:a', 'aac',
            '-shortest',
            output_path
        ])
        
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)

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
        
        self.preview_label = QLabel()
        self.preview_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.preview_label.setMinimumSize(640, 360)
        self.preview_label.setStyleSheet("background-color: black;")
        layout.addWidget(self.preview_label)
        
        self.time_label = QLabel()
        self.update_time_label()
        layout.addWidget(self.time_label)
        
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
        
        preview_layout = QHBoxLayout()
        preview_start_btn = QPushButton("Preview Start")
        preview_start_btn.clicked.connect(lambda: self.show_frame(self.start_frame))
        preview_layout.addWidget(preview_start_btn)
        
        preview_end_btn = QPushButton("Preview End")
        preview_end_btn.clicked.connect(lambda: self.show_frame(self.end_frame))
        preview_layout.addWidget(preview_end_btn)
        layout.addLayout(preview_layout)
        
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
        self.init_ui()
        self.setup_signals()
        self.load_clips_list()
        
        # Load audio devices
        if AUDIO_AVAILABLE:
            QTimer.singleShot(100, self.load_audio_devices)
        
        QTimer.singleShot(500, self.auto_start_recording)
        QTimer.singleShot(1000, self.auto_enable_hotkey)
        
    def init_ui(self):
        self.setWindowTitle("Screen Clip Recorder")
        self.setGeometry(100, 100, 1000, 800)
        
        central = QWidget()
        self.setCentralWidget(central)
        
        main_layout = QHBoxLayout()
        
        # Left panel
        left_panel = QVBoxLayout()
        
        # Status
        self.status_label = QLabel("Status: Starting...")
        left_panel.addWidget(self.status_label)
        
        # Audio settings group
        if AUDIO_AVAILABLE:
            audio_group = QGroupBox("Audio Settings")
            audio_layout = QVBoxLayout()
            
            # Microphone
            mic_layout = QHBoxLayout()
            self.mic_checkbox = QCheckBox("Record Microphone")
            self.mic_checkbox.stateChanged.connect(self.toggle_microphone)
            mic_layout.addWidget(self.mic_checkbox)
            audio_layout.addLayout(mic_layout)
            
            self.mic_combo = QComboBox()
            self.mic_combo.currentIndexChanged.connect(self.on_mic_device_changed)
            audio_layout.addWidget(self.mic_combo)
            
            # Desktop audio
            desktop_layout = QHBoxLayout()
            self.desktop_checkbox = QCheckBox("Record Desktop Audio")
            self.desktop_checkbox.stateChanged.connect(self.toggle_desktop_audio)
            desktop_layout.addWidget(self.desktop_checkbox)
            audio_layout.addLayout(desktop_layout)
            
            self.desktop_combo = QComboBox()
            self.desktop_combo.currentIndexChanged.connect(self.on_desktop_device_changed)
            audio_layout.addWidget(self.desktop_combo)
            
            audio_group.setLayout(audio_layout)
            left_panel.addWidget(audio_group)
        else:
            warning_label = QLabel("⚠️ Audio disabled: Install sounddevice and soundfile")
            warning_label.setStyleSheet("color: orange;")
            left_panel.addWidget(warning_label)
        
        # Buffer duration
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
        
        self.upload_btn = QPushButton("Upload")
        self.upload_btn.clicked.connect(self.upload_clip)
        clip_actions.addWidget(self.upload_btn)
        
        left_panel.addLayout(clip_actions)
        
        self.folder_btn = QPushButton("Open Clips Folder")
        self.folder_btn.clicked.connect(self.open_clips_folder)
        left_panel.addWidget(self.folder_btn)
        
        main_layout.addLayout(left_panel, 1)
        
        # Right panel
        self.viewer = ClipViewer()
        main_layout.addWidget(self.viewer, 2)
        
        central.setLayout(main_layout)
    
    def load_audio_devices(self):
        """Load available audio devices"""
        if not AUDIO_AVAILABLE or not self.recorder.audio_recorder:
            return
        
        input_devices, output_devices = self.recorder.audio_recorder.get_audio_devices()
        
        # Populate microphone dropdown
        self.mic_combo.clear()
        for device in input_devices:
            self.mic_combo.addItem(device['name'], device['index'])
        
        # Populate desktop audio dropdown
        # On Windows, look for "Stereo Mix" or similar
        # On Linux, look for "Monitor" devices
        # On macOS, this requires additional setup
        self.desktop_combo.clear()
        
        system = platform.system()
        if system == "Windows":
            # Look for stereo mix or loopback devices
            for device in input_devices:
                if 'stereo mix' in device['name'].lower() or 'wave out' in device['name'].lower():
                    self.desktop_combo.addItem(device['name'], device['index'])
        elif system == "Linux":
            # Look for monitor devices
            for device in input_devices:
                if 'monitor' in device['name'].lower() or 'output' in device['name'].lower():
                    self.desktop_combo.addItem(device['name'], device['index'])
        elif system == "Darwin":
            # macOS requires additional software like BlackHole
            self.desktop_combo.addItem("(Requires BlackHole or similar)", -1)
            for device in input_devices:
                if 'blackhole' in device['name'].lower() or 'soundflower' in device['name'].lower():
                    self.desktop_combo.addItem(device['name'], device['index'])
        
        if self.desktop_combo.count() == 0:
            self.desktop_combo.addItem("No desktop audio device found", -1)
            self.desktop_checkbox.setEnabled(False)
    
    def toggle_microphone(self, state):
        """Toggle microphone recording"""
        if not AUDIO_AVAILABLE or not self.recorder.audio_recorder:
            return
        
        enabled = state == Qt.CheckState.Checked.value
        self.recorder.audio_recorder.mic_enabled = enabled
        
        if enabled:
            device_index = self.mic_combo.currentData()
            if device_index is not None and device_index >= 0:
                self.recorder.audio_recorder.mic_device = device_index
                self.update_status("Microphone enabled")
            else:
                self.mic_checkbox.setChecked(False)
                self.show_error("Please select a microphone device")
        else:
            self.update_status("Microphone disabled")
        
        # Restart recording if active
        if self.recorder.is_recording:
            self.recorder.audio_recorder.stop_recording()
            self.recorder.audio_recorder.start_recording()
    
    def toggle_desktop_audio(self, state):
        """Toggle desktop audio recording"""
        if not AUDIO_AVAILABLE or not self.recorder.audio_recorder:
            return
        
        enabled = state == Qt.CheckState.Checked.value
        self.recorder.audio_recorder.desktop_enabled = enabled
        
        if enabled:
            device_index = self.desktop_combo.currentData()
            if device_index is not None and device_index >= 0:
                self.recorder.audio_recorder.desktop_device = device_index
                self.update_status("Desktop audio enabled")
            else:
                self.desktop_checkbox.setChecked(False)
                
                system = platform.system()
                if system == "Windows":
                    self.show_error(
                        "Desktop audio requires enabling Stereo Mix:\n\n"
                        "1. Right-click speaker icon in taskbar\n"
                        "2. Select 'Sounds' → 'Recording' tab\n"
                        "3. Right-click empty space → 'Show Disabled Devices'\n"
                        "4. Enable 'Stereo Mix' or 'Wave Out Mix'\n"
                        "5. Restart this application"
                    )
                elif system == "Linux":
                    self.show_error(
                        "Desktop audio requires PulseAudio monitor:\n\n"
                        "Run: pactl load-module module-loopback\n"
                        "Then restart this application"
                    )
                elif system == "Darwin":
                    self.show_error(
                        "Desktop audio on macOS requires BlackHole:\n\n"
                        "1. Install BlackHole: brew install blackhole-2ch\n"
                        "2. Configure Audio MIDI Setup to route audio\n"
                        "3. Restart this application"
                    )
        else:
            self.update_status("Desktop audio disabled")
        
        # Restart recording if active
        if self.recorder.is_recording:
            self.recorder.audio_recorder.stop_recording()
            self.recorder.audio_recorder.start_recording()
    
    def on_mic_device_changed(self, index):
        """Handle microphone device change"""
        if not AUDIO_AVAILABLE or not self.recorder.audio_recorder:
            return
        
        device_index = self.mic_combo.currentData()
        if device_index is not None and device_index >= 0:
            self.recorder.audio_recorder.mic_device = device_index
            if self.mic_checkbox.isChecked():
                if self.recorder.is_recording:
                    self.recorder.audio_recorder.stop_recording()
                    self.recorder.audio_recorder.start_recording()
    
    def on_desktop_device_changed(self, index):
        """Handle desktop audio device change"""
        if not AUDIO_AVAILABLE or not self.recorder.audio_recorder:
            return
        
        device_index = self.desktop_combo.currentData()
        if device_index is not None and device_index >= 0:
            self.recorder.audio_recorder.desktop_device = device_index
            if self.desktop_checkbox.isChecked():
                if self.recorder.is_recording:
                    self.recorder.audio_recorder.stop_recording()
                    self.recorder.audio_recorder.start_recording()
    
    def setup_signals(self):
        self.recorder.signals.clip_saved.connect(self.on_clip_saved)
        self.recorder.signals.status_update.connect(self.update_status)
        self.recorder.signals.error_occurred.connect(self.show_error)
        self.recorder.signals.permission_needed.connect(self.show_permission_dialog)
    
    def show_permission_dialog(self):
        msg = QMessageBox(self)
        msg.setWindowTitle("Screen Recording Permission Required")
        msg.setIcon(QMessageBox.Icon.Information)
        msg.setText(
            "<b>Screen Recording Permission Needed</b><br><br>"
            "This app needs permission to record your screen.<br><br>"
            "<b>⚠️ IMPORTANT: Do NOT run with sudo!</b><br>"
            "Running with sudo causes permission issues.<br><br>"
            "<b>To grant permission:</b><br>"
            "1. Quit this application completely (Cmd+Q)<br>"
            "2. Open <b>System Settings</b> (Apple menu → System Settings)<br>"
            "3. Go to <b>Privacy & Security</b> → <b>Screen Recording</b><br>"
            "4. Look for <b>Terminal</b>, <b>Python</b>, or <b>iTerm</b><br>"
            "5. Enable the checkbox next to it<br>"
            "6. Restart this application normally (without sudo)<br><br>"
            "<i>The permission change requires a complete restart of the app.</i>"
        )
        
        open_settings_btn = msg.addButton("Open System Settings", QMessageBox.ButtonRole.AcceptRole)
        quit_btn = msg.addButton("Quit App", QMessageBox.ButtonRole.DestructiveRole)
        msg.addButton("Later", QMessageBox.ButtonRole.RejectRole)
        
        result = msg.exec()
        
        if msg.clickedButton() == open_settings_btn:
            import subprocess
            subprocess.run([
                "open",
                "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"
            ])
            # Also show instructions
            QMessageBox.information(self, "Next Steps",
                "After enabling Screen Recording permission:\n\n"
                "1. Completely quit this app (Cmd+Q)\n"
                "2. Restart the app normally\n\n"
                "The permission will only work after a full restart."
            )
        elif msg.clickedButton() == quit_btn:
            QApplication.quit()
            sys.exit(0)
        
        self.update_status("Waiting for screen recording permission...")
        self.start_btn.setText("Start Recording Buffer")
        self.start_btn.setEnabled(False)
    
    def auto_start_recording(self):
        if not self.recorder.is_recording:
            # On macOS, be extra careful about auto-starting
            if platform.system() == "Darwin":
                # Only auto-start if we haven't shown the permission dialog
                if hasattr(self.recorder, 'permission_granted') and not self.recorder.permission_granted:
                    self.update_status("Screen recording permission needed - click 'Start Recording Buffer' to begin")
                    self.start_btn.setText("Start Recording Buffer")
                    self.start_btn.setEnabled(True)
                    return
            
            success = self.recorder.start_recording()
            if success:
                self.update_status("Recording buffer active (auto-started)")
            else:
                self.start_btn.setEnabled(True)
    
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
    
    def upload_clip(self):
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
        
        file_size = clip_path.stat().st_size
        file_size_mb = file_size / (1024 * 1024)
        
        if not hasattr(self, 'server_url') or not self.server_url:
            self.server_url = self.prompt_server_url()
        
        if not self.server_url:
            return
        
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
        dialog = QInputDialog(self)
        dialog.setWindowTitle("Server URL")
        dialog.setLabelText(
            "Enter your Screen Clips server URL:\n\n"
            "Examples:\n"
            "• https://your-app.railway.app\n"
            "• https://your-app.onrender.com\n"
            "• http://localhost:5000"
        )
        dialog.setTextValue("https://your-app.railway.app")
        dialog.resize(500, 200)
        
        if dialog.exec() == QInputDialog.DialogCode.Accepted:
            url = dialog.textValue().strip().rstrip('/')
            if url:
                if not url.startswith('http://') and not url.startswith('https://'):
                    self.show_error("URL must start with http:// or https://")
                    return self.prompt_server_url()
                return url
        
        return None
    
    def show_upload_success(self, clip_url, direct_url, filename):
        msg = QMessageBox(self)
        msg.setWindowTitle("Upload Successful!")
        msg.setIcon(QMessageBox.Icon.Information)
        
        message = f"""
<b>✓ Clip uploaded successfully!</b><br><br>
<b>File:</b> {filename}<br>
<b>Watch URL:</b> <a href="{clip_url}">{clip_url}</a><br><br>
<i>The URL has been copied to your clipboard.</i>
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
    
    def rename_clip(self):
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
            
            self.update_status(f"Trimmed clip saved")
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
                                      f"The file is still being used.\n"
                                      f"Close any video players viewing this file.")
                except Exception as e:
                    self.show_error(f"Failed to delete: {str(e)}")
                    return
    
    def open_clips_folder(self):
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
        self.status_label.setText(f"Status: {message}")
    
    def show_error(self, message):
        QMessageBox.critical(self, "Error", message)
    
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
    # Check if running with sudo on macOS
    if platform.system() == "Darwin":
        if os.geteuid() == 0:
            print("\n" + "="*60)
            print("⚠️  WARNING: Running with sudo is NOT recommended!")
            print("="*60)
            print("\nThis causes screen recording permission issues on macOS.")
            print("\nPlease:")
            print("1. Quit this app (Ctrl+C)")
            print("2. Run without sudo: python screen_recorder.py")
            print("3. Grant screen recording permission when prompted")
            print("\n" + "="*60 + "\n")
            
            response = input("Continue anyway? (not recommended) [y/N]: ")
            if response.lower() != 'y':
                print("Exiting. Please run without sudo.")
                sys.exit(1)
    
    app = QApplication(sys.argv)
    
    # macOS specific: Check for screen recording permission before showing UI
    if platform.system() == "Darwin":
        print("\nChecking screen recording permissions...")
        print("Note: A system dialog may appear requesting permission.")
        print("This is normal on first run.\n")
    
    window = MainWindow()
    window.show()
    sys.exit(app.exec())

if __name__ == "__main__":
    main()
