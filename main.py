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
                             QMessageBox, QSlider, QSpinBox,
                             QDialog, QDialogButtonBox, QLineEdit, QInputDialog,
                             QCheckBox, QComboBox, QGroupBox)
from PyQt6.QtCore import QTimer, Qt, pyqtSignal, QObject, QThread
from PyQt6.QtGui import QPixmap, QImage, QKeySequence
import platform
import os
import tempfile
import subprocess

# Audio recording imports - these are optional libraries
try:
    import sounddevice as sd
    import soundfile as sf
    AUDIO_AVAILABLE = True
except ImportError:
    AUDIO_AVAILABLE = False
    print("Warning: sounddevice/soundfile not available. Audio recording disabled.")
    print("Install with: pip install sounddevice soundfile")

# Try to import pynput for cross-platform hotkey support (works without admin on macOS)
try:
    from pynput import keyboard as pynput_keyboard
    PYNPUT_AVAILABLE = True
except ImportError:
    PYNPUT_AVAILABLE = False
    print("Warning: pynput not available. Hotkeys may not work on macOS.")
    print("Install with: pip install pynput")

class RecorderSignals(QObject):
    """
    Qt signals for thread-safe communication between worker threads and the UI.
    This allows background threads to safely update the GUI.
    """
    clip_saved = pyqtSignal(str)  # Emitted when a clip is successfully saved
    status_update = pyqtSignal(str)  # Emitted to update the status label
    error_occurred = pyqtSignal(str)  # Emitted when an error occurs
    permission_needed = pyqtSignal()  # Emitted when screen recording permission is needed

class AudioRecorder:
    """
    Handles audio recording from microphone and/or desktop audio.
    Uses a circular buffer to store the most recent audio data.
    """
    
    def __init__(self, sample_rate=44100):
        """
        Initialize the audio recorder.
        
        Args:
            sample_rate: Audio sample rate in Hz (44100 is CD quality)
        """
        self.sample_rate = sample_rate
        self.is_recording = False
        
        # Flags to enable/disable each audio source
        self.mic_enabled = False
        self.desktop_enabled = False
        
        # Device indices for the selected audio devices
        self.mic_device = None
        self.desktop_device = None
        
        # Circular buffer to store recent audio (stores last 5 minutes)
        # Each entry is a chunk of audio data
        self.audio_buffer = deque(maxlen=300)
        
        # Thread for continuous audio recording
        self.record_thread = None
        
        # Lock to prevent race conditions when accessing the buffer
        self.lock = threading.Lock()
        
    def get_audio_devices(self):
        """
        Query the system for available audio input and output devices.
        
        Returns:
            tuple: (input_devices, output_devices) - lists of device dictionaries
        """
        if not AUDIO_AVAILABLE:
            return [], []
        
        try:
            devices = sd.query_devices()
            input_devices = []
            output_devices = []
            
            # Iterate through all devices and categorize them
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
        """
        Start continuous audio recording in a background thread.
        
        Returns:
            bool: True if recording started successfully
        """
        if not AUDIO_AVAILABLE:
            return False
        
        # Only start if at least one audio source is enabled
        if not self.mic_enabled and not self.desktop_enabled:
            return False
        
        self.is_recording = True
        self.record_thread = threading.Thread(target=self._record_loop, daemon=True)
        self.record_thread.start()
        return True
    
    def stop_recording(self):
        """Stop the audio recording thread."""
        self.is_recording = False
        if self.record_thread:
            self.record_thread.join(timeout=2)  # Wait up to 2 seconds for thread to finish
    
    def _record_loop(self):
        """
        Main audio recording loop (runs in a background thread).
        Opens audio streams and continuously records audio.
        """
        streams = []
        
        try:
            # Open microphone stream if enabled
            if self.mic_enabled and self.mic_device is not None:
                try:
                    mic_stream = sd.InputStream(
                        device=self.mic_device,
                        channels=1,  # Mono audio
                        samplerate=self.sample_rate,
                        blocksize=int(self.sample_rate * 0.1),  # 100ms chunks
                        callback=self._audio_callback
                    )
                    streams.append(mic_stream)
                    mic_stream.start()
                    print(f"Started microphone recording on device {self.mic_device}")
                except Exception as e:
                    print(f"Failed to open mic stream: {e}")
            
            # Open desktop audio stream if enabled
            if self.desktop_enabled and self.desktop_device is not None:
                try:
                    # Desktop audio is typically stereo (2 channels)
                    desktop_stream = sd.InputStream(
                        device=self.desktop_device,
                        channels=2,  # Stereo audio
                        samplerate=self.sample_rate,
                        blocksize=int(self.sample_rate * 0.1),  # 100ms chunks
                        callback=self._audio_callback
                    )
                    streams.append(desktop_stream)
                    desktop_stream.start()
                    print(f"Started desktop audio recording on device {self.desktop_device}")
                except Exception as e:
                    print(f"Failed to open desktop stream: {e}")
            
            # Keep the recording loop running
            while self.is_recording:
                time.sleep(0.1)
            
        except Exception as e:
            print(f"Audio recording error: {e}")
        finally:
            # Clean up: stop and close all audio streams
            for stream in streams:
                try:
                    stream.stop()
                    stream.close()
                except:
                    pass
    
    def _audio_callback(self, indata, frames, time_info, status):
        """
        Callback function called by sounddevice when new audio data is available.
        This runs in a separate thread managed by sounddevice.
        
        Args:
            indata: numpy array containing audio samples
            frames: number of frames
            time_info: timing information
            status: status flags
        """
        if status:
            print(f"Audio status: {status}")
        
        with self.lock:
            # Convert stereo to mono if needed (average the two channels)
            if len(indata.shape) > 1 and indata.shape[1] == 2:
                audio_data = np.mean(indata, axis=1, keepdims=True)
            else:
                audio_data = indata.copy()
            
            # Add this chunk to the circular buffer
            self.audio_buffer.append(audio_data)
    
    def get_audio_data(self, duration_seconds):
        """
        Get the last N seconds of recorded audio.
        
        Args:
            duration_seconds: How many seconds of audio to retrieve
            
        Returns:
            numpy array of audio samples, or None if no audio available
        """
        with self.lock:
            if not self.audio_buffer:
                return None
            
            # Calculate how many samples we need for the requested duration
            samples_needed = int(duration_seconds * self.sample_rate)
            
            # Get all buffered audio chunks
            all_data = list(self.audio_buffer)
            if not all_data:
                return None
            
            # Concatenate all chunks into a single array
            audio_array = np.concatenate(all_data, axis=0)
            
            # Take only the last duration_seconds worth of audio
            if len(audio_array) > samples_needed:
                audio_array = audio_array[-samples_needed:]
            
            return audio_array
    
    def clear_buffers(self):
        """Clear all audio from the buffer."""
        with self.lock:
            self.audio_buffer.clear()

class ScreenRecorder:
    """
    Handles continuous screen recording and saving clips.
    Maintains a circular buffer of recent frames that can be saved on demand.
    """
    
    def __init__(self, buffer_seconds=30, fps=30):
        """
        Initialize the screen recorder.
        
        Args:
            buffer_seconds: How many seconds of video to keep in the buffer
            fps: Frames per second to record
        """
        self.buffer_seconds = buffer_seconds
        self.fps = fps
        
        # Circular buffer to store recent frames
        # maxlen ensures it automatically discards old frames
        self.frame_buffer = deque(maxlen=buffer_seconds * fps)
        
        self.is_recording = False
        self.record_thread = None
        self.signals = RecorderSignals()
        
        # Directory to save clips (creates ~/ScreenClips/)
        self.clips_dir = Path.home() / "ScreenClips"
        self.clips_dir.mkdir(exist_ok=True)
        
        self.permission_granted = False
        self.sct = None  # mss screenshot object
        
        # Create audio recorder if audio libraries are available
        self.audio_recorder = AudioRecorder() if AUDIO_AVAILABLE else None
        
    def check_screen_recording_permission(self):
        """
        Check if the app has permission to record the screen (macOS only).
        On other platforms, this always returns True.
        
        Returns:
            bool: True if permission is granted
        """
        # Only macOS requires explicit screen recording permission
        if platform.system() != "Darwin":
            return True
        
        # Cache the result to avoid repeated checks
        if hasattr(self, '_mac_permission_checked') and self._mac_permission_checked:
            return self.permission_granted
        
        self._mac_permission_checked = True
        
        try:
            if not hasattr(self, '_permission_test_done'):
                self._permission_test_done = True
                
                # Try to capture a small portion of the screen
                with mss.mss() as sct:
                    monitor = sct.monitors[1]
                    test_region = {
                        "top": monitor["top"],
                        "left": monitor["left"],
                        "width": 10,
                        "height": 10
                    }
                    screenshot = sct.grab(test_region)
                    img = np.array(screenshot)
                    
                    # If we get actual pixel data (not all black), permission is granted
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
        """
        Start continuous screen recording in a background thread.
        
        Returns:
            bool: True if recording started successfully
        """
        # Check for screen recording permission on macOS
        if platform.system() == "Darwin":
            if not hasattr(self, '_initial_permission_check_done'):
                self._initial_permission_check_done = True
                if not self.check_screen_recording_permission():
                    self.signals.permission_needed.emit()
                    return False
                time.sleep(0.5)  # Brief delay to ensure permission is fully granted
        
        if not self.is_recording:
            self.is_recording = True
            self.record_thread = threading.Thread(target=self._record_loop, daemon=True)
            self.record_thread.start()
            
            # Start audio recording if audio recorder exists
            if self.audio_recorder:
                self.audio_recorder.start_recording()
            
            self.signals.status_update.emit("Recording active")
            return True
        return True
    
    def stop_recording(self):
        """Stop the screen recording thread."""
        self.is_recording = False
        if self.record_thread:
            self.record_thread.join(timeout=2)
        
        # Stop audio recording
        if self.audio_recorder:
            self.audio_recorder.stop_recording()
        
        # Clean up the screenshot object
        if self.sct:
            try:
                self.sct.close()
            except:
                pass
            self.sct = None
        
        self.signals.status_update.emit("Recording stopped")
    
    def _record_loop(self):
        """
        Main screen recording loop (runs in a background thread).
        Continuously captures screenshots and adds them to the buffer.
        """
        try:
            # Create a persistent mss instance for this thread
            self.sct = mss.mss()
            monitor = self.sct.monitors[1]  # Primary monitor
            
            # Linux-specific monitor setup
            if platform.system() == "Linux":
                monitor = {
                    "top": 0,
                    "left": 0,
                    "width": monitor["width"],
                    "height": monitor["height"],
                    "mon": 1
                }
            
            # Calculate delay between frames to achieve target FPS
            frame_delay = 1.0 / self.fps
            consecutive_errors = 0
            max_consecutive_errors = 10
            
            while self.is_recording:
                start_time = time.time()
                
                try:
                    # Capture the screen
                    screenshot = self.sct.grab(monitor)
                    frame = np.array(screenshot)
                    
                    # macOS: Check if we got black/empty frames (permission issue)
                    if platform.system() == "Darwin":
                        if frame.size == 0 or np.all(frame == 0):
                            consecutive_errors += 1
                            if consecutive_errors >= max_consecutive_errors:
                                self.signals.error_occurred.emit(
                                    "Screen recording permission denied.\n\n"
                                    "Grant permission in System Settings:\n"
                                    "Privacy & Security → Screen Recording"
                                )
                                self.is_recording = False
                                break
                            time.sleep(0.5)
                            continue
                    
                    consecutive_errors = 0  # Reset error counter on success
                    
                    # Convert color format to BGR (OpenCV format)
                    if frame.shape[2] == 4:  # BGRA
                        frame = cv2.cvtColor(frame, cv2.COLOR_BGRA2BGR)
                    elif frame.shape[2] == 3:  # RGB
                        frame = cv2.cvtColor(frame, cv2.COLOR_RGB2BGR)
                    
                    # Add frame and timestamp to buffer
                    timestamp = datetime.now()
                    self.frame_buffer.append((frame, timestamp))
                    
                except Exception as e:
                    consecutive_errors += 1
                    
                    # If too many errors, stop recording
                    if consecutive_errors >= max_consecutive_errors:
                        self.signals.error_occurred.emit(f"Recording error: {str(e)}")
                        self.is_recording = False
                        break
                    
                    time.sleep(0.5)
                    continue
                
                # Sleep to maintain target FPS
                elapsed = time.time() - start_time
                sleep_time = max(0, frame_delay - elapsed)
                time.sleep(sleep_time)
            
            # Clean up
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
        """
        Save the last N seconds from the buffer as a video file.
        
        Args:
            duration_seconds: How many seconds to save (uses buffer_seconds if None)
            
        Returns:
            str: Path to saved file, or None if failed
        """
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
        
        # Get the last N frames from the buffer
        frames_list = list(self.frame_buffer)[-frames_to_save:]
        
        # Get corresponding audio if available
        audio_data = None
        if self.audio_recorder and AUDIO_AVAILABLE:
            audio_data = self.audio_recorder.get_audio_data(duration_seconds)
        
        # Generate filename with timestamp
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        readable_time = datetime.now().strftime("%Y-%m-%d %I:%M:%S %p")
        filename = self.clips_dir / f"clip_{timestamp}.mp4"
        metadata_file = self.clips_dir / f"clip_{timestamp}.meta"
        
        # Save video in a background thread to avoid blocking the UI
        threading.Thread(target=self._save_video, 
                        args=(frames_list, filename, metadata_file, readable_time, audio_data), 
                        daemon=True).start()
        
        return str(filename)
    
    def _save_video(self, frames_list, filename, metadata_file, readable_time, audio_data):
        """
        Save frames and audio to a video file (runs in background thread).
        
        Args:
            frames_list: List of (frame, timestamp) tuples
            filename: Path to save video file
            metadata_file: Path to save metadata file
            readable_time: Human-readable timestamp string
            audio_data: numpy array of audio samples, or None
        """
        try:
            if not frames_list:
                return
            
            # Get dimensions from first frame
            height, width = frames_list[0][0].shape[:2]
            
            # Create temporary file for video (without audio)
            temp_video = tempfile.NamedTemporaryFile(suffix='.mp4', delete=False)
            temp_video_path = temp_video.name
            temp_video.close()
            
            # Create video writer
            fourcc = cv2.VideoWriter_fourcc(*'mp4v')
            out = cv2.VideoWriter(temp_video_path, fourcc, self.fps, (width, height))
            
            # Write all frames to video
            for frame, _ in frames_list:
                out.write(frame)
            
            out.release()
            
            # Process audio if available
            has_audio = False
            temp_audio_path = None
            
            if AUDIO_AVAILABLE and audio_data is not None and len(audio_data) > 0:
                try:
                    # Save audio to temporary WAV file
                    temp_audio_path = tempfile.NamedTemporaryFile(suffix='.wav', delete=False).name
                    sf.write(temp_audio_path, audio_data, self.audio_recorder.sample_rate)
                    has_audio = True
                except Exception as e:
                    print(f"Audio processing error: {e}")
            
            # Combine video and audio using ffmpeg (if available)
            if has_audio and self._check_ffmpeg():
                try:
                    self._merge_video_audio(temp_video_path, temp_audio_path, str(filename))
                except Exception as e:
                    print(f"FFmpeg merge error: {e}")
                    # Fall back to video only
                    import shutil
                    shutil.move(temp_video_path, str(filename))
            else:
                # No audio or ffmpeg not available - save video only
                import shutil
                shutil.move(temp_video_path, str(filename))
            
            # Clean up temporary files
            try:
                if os.path.exists(temp_video_path):
                    os.unlink(temp_video_path)
                if temp_audio_path and os.path.exists(temp_audio_path):
                    os.unlink(temp_audio_path)
            except:
                pass
            
            # Save metadata file (for custom clip names)
            with open(metadata_file, 'w') as f:
                f.write(f"timestamp={readable_time}\n")
                f.write(f"name=\n")
            
            # Notify UI that clip was saved
            self.signals.clip_saved.emit(str(filename))
            self.signals.status_update.emit(f"Clip saved: {filename.name}")
            
        except Exception as e:
            self.signals.error_occurred.emit(f"Error saving clip: {str(e)}")
    
    def _check_ffmpeg(self):
        """
        Check if ffmpeg is available on the system.
        
        Returns:
            bool: True if ffmpeg is available
        """
        try:
            subprocess.run(['ffmpeg', '-version'], 
                         stdout=subprocess.DEVNULL, 
                         stderr=subprocess.DEVNULL)
            return True
        except FileNotFoundError:
            return False
    
    def _merge_video_audio(self, video_path, audio_path, output_path):
        """
        Merge video and audio files using ffmpeg.
        
        Args:
            video_path: Path to video file
            audio_path: Path to audio file
            output_path: Path to save merged file
        """
        cmd = [
            'ffmpeg', '-y',  # Overwrite output file
            '-i', video_path,  # Input video
            '-i', audio_path,  # Input audio
            '-c:v', 'copy',  # Copy video codec (no re-encoding)
            '-c:a', 'aac',  # Encode audio as AAC
            '-shortest',  # Use shortest stream duration
            output_path
        ]
        
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)

class HotkeyListener(QThread):
    """
    Cross-platform hotkey listener that works without admin privileges on macOS.
    Uses pynput library for global hotkey detection.
    """
    
    # Signal emitted when the hotkey is pressed
    hotkey_pressed = pyqtSignal()
    
    def __init__(self, hotkey_combination):
        """
        Initialize the hotkey listener.
        
        Args:
            hotkey_combination: String describing the hotkey (e.g., "F9", "Ctrl+Shift+S")
        """
        super().__init__()
        self.hotkey_combination = hotkey_combination
        self.is_running = False
        self.listener = None
        
    def run(self):
        """Main thread loop - listens for hotkey presses."""
        if not PYNPUT_AVAILABLE:
            return
        
        self.is_running = True
        
        try:
            # Parse the hotkey combination
            hotkey = self._parse_hotkey(self.hotkey_combination)
            
            # Create and start the global hotkey listener
            with pynput_keyboard.GlobalHotKeys({hotkey: self._on_hotkey}) as listener:
                self.listener = listener
                while self.is_running:
                    time.sleep(0.1)
        except Exception as e:
            print(f"Hotkey listener error: {e}")
    
    def _parse_hotkey(self, combination):
        """
        Parse hotkey combination string into pynput format.
        
        Args:
            combination: String like "F9" or "Ctrl+Shift+S"
            
        Returns:
            str: Formatted hotkey string for pynput
        """
        # Convert to pynput format (e.g., "<ctrl>+<shift>+s")
        parts = combination.lower().split('+')
        result = []
        
        for part in parts:
            part = part.strip()
            if part in ['ctrl', 'control', 'cmd', 'command']:
                result.append('<ctrl>' if platform.system() != 'Darwin' else '<cmd>')
            elif part in ['shift']:
                result.append('<shift>')
            elif part in ['alt', 'option']:
                result.append('<alt>')
            elif part.startswith('f') and len(part) <= 3:  # Function keys (F1-F12)
                result.append(f'<{part}>')
            else:
                result.append(part)
        
        return '+'.join(result)
    
    def _on_hotkey(self):
        """Called when the hotkey is pressed."""
        self.hotkey_pressed.emit()
    
    def stop(self):
        """Stop the hotkey listener."""
        self.is_running = False
        if self.listener:
            try:
                self.listener.stop()
            except:
                pass

class HotkeyDialog(QDialog):
    """
    Dialog for customizing the hotkey.
    Allows user to choose from preset hotkeys or create a custom one.
    """
    
    def __init__(self, current_hotkey, parent=None):
        super().__init__(parent)
        self.current_hotkey = current_hotkey
        self.selected_hotkey = current_hotkey
        self.init_ui()
        
    def init_ui(self):
        """Initialize the dialog UI."""
        self.setWindowTitle("Customize Hotkey")
        self.setModal(True)
        
        layout = QVBoxLayout()
        
        # Instruction label
        layout.addWidget(QLabel("Choose a hotkey to save clips:"))
        
        # Radio buttons for preset hotkeys
        self.preset_combo = QComboBox()
        preset_hotkeys = ["F9", "F10", "F11", "F12", "Ctrl+Shift+S", "Ctrl+Shift+R"]
        self.preset_combo.addItems(preset_hotkeys)
        
        # Set current hotkey if it's in presets
        if self.current_hotkey in preset_hotkeys:
            self.preset_combo.setCurrentText(self.current_hotkey)
        
        layout.addWidget(QLabel("Preset Hotkeys:"))
        layout.addWidget(self.preset_combo)
        
        # Custom hotkey input
        layout.addWidget(QLabel("Or enter custom hotkey:"))
        self.custom_input = QLineEdit()
        self.custom_input.setPlaceholderText("e.g., F8, Ctrl+S, Ctrl+Shift+C")
        layout.addWidget(self.custom_input)
        
        # Buttons
        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | 
            QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        
        self.setLayout(layout)
    
    def accept(self):
        """Handle OK button click."""
        # Use custom hotkey if provided, otherwise use preset
        custom = self.custom_input.text().strip()
        if custom:
            self.selected_hotkey = custom
        else:
            self.selected_hotkey = self.preset_combo.currentText()
        
        super().accept()
    
    def get_hotkey(self):
        """
        Get the selected hotkey.
        
        Returns:
            str: Selected hotkey combination
        """
        return self.selected_hotkey

class DurationDialog(QDialog):
    """
    Dialog for setting custom clip duration.
    Allows user to specify minutes and seconds.
    """
    
    def __init__(self, parent=None):
        super().__init__(parent)
        self.init_ui()
        
    def init_ui(self):
        """Initialize the dialog UI."""
        self.setWindowTitle("Custom Duration")
        self.setModal(True)
        
        layout = QVBoxLayout()
        
        # Minutes input
        minutes_layout = QHBoxLayout()
        minutes_layout.addWidget(QLabel("Minutes:"))
        self.minutes_spin = QSpinBox()
        self.minutes_spin.setRange(0, 10)
        self.minutes_spin.setValue(0)
        minutes_layout.addWidget(self.minutes_spin)
        layout.addLayout(minutes_layout)
        
        # Seconds input
        seconds_layout = QHBoxLayout()
        seconds_layout.addWidget(QLabel("Seconds:"))
        self.seconds_spin = QSpinBox()
        self.seconds_spin.setRange(0, 59)
        self.seconds_spin.setValue(30)
        seconds_layout.addWidget(self.seconds_spin)
        layout.addLayout(seconds_layout)
        
        # Total duration label
        self.total_label = QLabel()
        self.update_total_label()
        layout.addWidget(self.total_label)
        
        # Connect signals to update total
        self.minutes_spin.valueChanged.connect(self.update_total_label)
        self.seconds_spin.valueChanged.connect(self.update_total_label)
        
        # Buttons
        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | 
            QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        
        self.setLayout(layout)
    
    def update_total_label(self):
        """Update the total duration label."""
        total_seconds = self.minutes_spin.value() * 60 + self.seconds_spin.value()
        self.total_label.setText(f"Total: {total_seconds} seconds")
    
    def get_duration(self):
        """
        Get the selected duration in seconds.
        
        Returns:
            int: Total duration in seconds
        """
        return self.minutes_spin.value() * 60 + self.seconds_spin.value()

class TrimDialog(QDialog):
    """
    Dialog for trimming video clips.
    Allows user to select start and end points in the video.
    """
    
    def __init__(self, video_path, parent=None):
        super().__init__(parent)
        self.video_path = video_path
        
        # Open video file and get properties
        self.cap = cv2.VideoCapture(video_path)
        self.total_frames = int(self.cap.get(cv2.CAP_PROP_FRAME_COUNT))
        self.fps = self.cap.get(cv2.CAP_PROP_FPS)
        self.duration = self.total_frames / self.fps
        
        # Start and end frames for trimming
        self.start_frame = 0
        self.end_frame = self.total_frames - 1
        
        self.init_ui()
        
    def init_ui(self):
        """Initialize the dialog UI."""
        self.setWindowTitle("Trim Clip")
        self.setGeometry(200, 200, 800, 600)
        
        layout = QVBoxLayout()
        
        # Video preview label
        self.preview_label = QLabel()
        self.preview_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.preview_label.setMinimumSize(640, 360)
        self.preview_label.setStyleSheet("background-color: black;")
        layout.addWidget(self.preview_label)
        
        # Duration label
        self.time_label = QLabel()
        self.update_time_label()
        layout.addWidget(self.time_label)
        
        # Start time slider
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
        
        # End time slider
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
        self.show_frame(0)  # Show first frame initially
    
    def on_start_changed(self, value):
        """Handle start slider change."""
        # Ensure start is before end
        if value >= self.end_frame:
            value = self.end_frame - 1
            self.start_slider.setValue(value)
        
        self.start_frame = value
        self.start_time_label.setText(f"{value / self.fps:.1f}s")
        self.update_time_label()
        self.show_frame(value)
    
    def on_end_changed(self, value):
        """Handle end slider change."""
        # Ensure end is after start
        if value <= self.start_frame:
            value = self.start_frame + 1
            self.end_slider.setValue(value)
        
        self.end_frame = value
        self.end_time_label.setText(f"{value / self.fps:.1f}s")
        self.update_time_label()
        self.show_frame(value)
    
    def update_time_label(self):
        """Update the trimmed duration label."""
        duration = (self.end_frame - self.start_frame) / self.fps
        self.time_label.setText(f"Trimmed Duration: {duration:.1f}s")
    
    def show_frame(self, frame_num):
        """
        Display a specific frame in the preview.
        
        Args:
            frame_num: Frame number to display
        """
        self.cap.set(cv2.CAP_PROP_POS_FRAMES, frame_num)
        ret, frame = self.cap.read()
        
        if ret:
            # Convert BGR to RGB for Qt display
            rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            h, w, ch = rgb_frame.shape
            bytes_per_line = ch * w
            q_img = QImage(rgb_frame.data, w, h, bytes_per_line, QImage.Format.Format_RGB888)
            
            # Scale to fit preview area
            pixmap = QPixmap.fromImage(q_img)
            scaled_pixmap = pixmap.scaled(self.preview_label.size(), 
                                          Qt.AspectRatioMode.KeepAspectRatio,
                                          Qt.TransformationMode.SmoothTransformation)
            self.preview_label.setPixmap(scaled_pixmap)
    
    def get_trim_range(self):
        """
        Get the selected trim range.
        
        Returns:
            tuple: (start_frame, end_frame)
        """
        return self.start_frame, self.end_frame
    
    def closeEvent(self, event):
        """Clean up when dialog is closed."""
        if self.cap:
            self.cap.release()
        event.accept()

class ClipViewer(QWidget):
    """
    Widget for viewing and playing saved clips.
    Includes playback controls and timeline scrubbing.
    """
    
    def __init__(self):
        super().__init__()
        self.current_clip_path = None
        self.cap = None
        self.init_ui()
        
    def init_ui(self):
        """Initialize the viewer UI."""
        layout = QVBoxLayout()
        
        # Video display label
        self.video_label = QLabel("No clip loaded")
        self.video_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.video_label.setMinimumSize(640, 480)
        self.video_label.setStyleSheet("background-color: black; color: white;")
        layout.addWidget(self.video_label)
        
        # Time display
        self.time_label = QLabel("0:00 / 0:00")
        self.time_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self.time_label)
        
        # Playback controls
        controls = QHBoxLayout()
        
        self.play_btn = QPushButton("Play")
        self.play_btn.clicked.connect(self.toggle_play)
        self.play_btn.setEnabled(False)
        controls.addWidget(self.play_btn)
        
        # Timeline slider
        self.position_slider = QSlider(Qt.Orientation.Horizontal)
        self.position_slider.setEnabled(False)
        self.position_slider.sliderMoved.connect(self.seek)
        controls.addWidget(self.position_slider)
        
        layout.addLayout(controls)
        
        # Timer for playback updates
        self.timer = QTimer()
        self.timer.timeout.connect(self.update_frame)
        self.is_playing = False
        
        self.setLayout(layout)
    
    def release_current_clip(self):
        """Release the current clip and clean up resources."""
        # Stop playback
        if self.is_playing:
            self.is_playing = False
            self.timer.stop()
            self.play_btn.setText("Play")
        
        # Release video capture
        if self.cap:
            self.cap.release()
            self.cap = None
        
        # Force garbage collection to free memory
        import gc
        gc.collect()
        
        # Reset UI
        self.video_label.clear()
        self.video_label.setText("No clip loaded")
        self.time_label.setText("0:00 / 0:00")
        self.position_slider.setValue(0)
        self.play_btn.setEnabled(False)
        self.position_slider.setEnabled(False)
        
        self.current_clip_path = None
    
    def load_clip(self, filepath):
        """
        Load a video clip for viewing.
        
        Args:
            filepath: Path to the video file
        """
        # Release any currently loaded clip
        self.release_current_clip()
        
        self.current_clip_path = filepath
        self.cap = cv2.VideoCapture(filepath)
        
        if self.cap.isOpened():
            # Enable controls
            self.play_btn.setEnabled(True)
            self.position_slider.setEnabled(True)
            
            # Set slider range
            total_frames = int(self.cap.get(cv2.CAP_PROP_FRAME_COUNT))
            self.position_slider.setMaximum(total_frames - 1)
            
            # Show first frame
            self.show_frame(0)
            self.update_time_display()
        
    def toggle_play(self):
        """Toggle between play and pause."""
        if not self.cap:
            return
        
        self.is_playing = not self.is_playing
        
        if self.is_playing:
            self.play_btn.setText("Pause")
            # Start timer at video's FPS
            fps = self.cap.get(cv2.CAP_PROP_FPS)
            self.timer.start(int(1000 / fps))
        else:
            self.play_btn.setText("Play")
            self.timer.stop()
    
    def update_frame(self):
        """Update to next frame during playback."""
        if not self.cap:
            return
        
        ret, frame = self.cap.read()
        
        if ret:
            self.display_frame(frame)
            current_pos = int(self.cap.get(cv2.CAP_PROP_POS_FRAMES))
            self.position_slider.setValue(current_pos)
            self.update_time_display()
        else:
            # End of video - stop playback and reset
            self.is_playing = False
            self.play_btn.setText("Play")
            self.timer.stop()
            self.cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
            self.position_slider.setValue(0)
    
    def show_frame(self, frame_num):
        """
        Show a specific frame.
        
        Args:
            frame_num: Frame number to display
        """
        if not self.cap:
            return
        
        self.cap.set(cv2.CAP_PROP_POS_FRAMES, frame_num)
        ret, frame = self.cap.read()
        
        if ret:
            self.display_frame(frame)
    
    def seek(self, position):
        """
        Seek to a specific position in the video.
        
        Args:
            position: Frame number to seek to
        """
        self.show_frame(position)
        self.update_time_display()
    
    def display_frame(self, frame):
        """
        Display a frame in the video label.
        
        Args:
            frame: OpenCV frame (BGR format)
        """
        # Convert BGR to RGB
        rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        h, w, ch = rgb_frame.shape
        bytes_per_line = ch * w
        q_img = QImage(rgb_frame.data, w, h, bytes_per_line, QImage.Format.Format_RGB888)
        
        # Scale to fit display area
        pixmap = QPixmap.fromImage(q_img)
        scaled_pixmap = pixmap.scaled(self.video_label.size(), 
                                      Qt.AspectRatioMode.KeepAspectRatio,
                                      Qt.TransformationMode.SmoothTransformation)
        self.video_label.setPixmap(scaled_pixmap)
    
    def update_time_display(self):
        """Update the time display label."""
        if not self.cap:
            return
        
        current_frame = int(self.cap.get(cv2.CAP_PROP_POS_FRAMES))
        total_frames = int(self.cap.get(cv2.CAP_PROP_FRAME_COUNT))
        fps = self.cap.get(cv2.CAP_PROP_FPS)
        
        # Convert frames to time
        current_time = current_frame / fps if fps > 0 else 0
        total_time = total_frames / fps if fps > 0 else 0
        
        # Format as MM:SS
        current_str = f"{int(current_time // 60)}:{int(current_time % 60):02d}"
        total_str = f"{int(total_time // 60)}:{int(total_time % 60):02d}"
        
        self.time_label.setText(f"{current_str} / {total_str}")

class MainWindow(QMainWindow):
    """
    Main application window.
    Contains all UI elements and coordinates the recorder, viewer, and controls.
    """
    
    def __init__(self):
        super().__init__()
        
        # Create screen recorder
        self.recorder = ScreenRecorder(buffer_seconds=30, fps=30)
        
        # Hotkey settings
        self.current_hotkey = "F9"
        self.hotkey_listener = None
        
        # Initialize UI
        self.init_ui()
        self.setup_signals()
        self.load_clips_list()
        
        # Load audio devices if available
        if AUDIO_AVAILABLE:
            QTimer.singleShot(100, self.load_audio_devices)
        
        # Auto-start recording after brief delay
        QTimer.singleShot(500, self.auto_start_recording)
        
        # Enable hotkey after startup
        QTimer.singleShot(1000, self.start_hotkey_listener)
        
    def init_ui(self):
        """Initialize the main window UI."""
        self.setWindowTitle("Screen Clip Recorder")
        self.setGeometry(100, 100, 1000, 800)
        
        central = QWidget()
        self.setCentralWidget(central)
        
        main_layout = QHBoxLayout()
        
        # ===== LEFT PANEL =====
        left_panel = QVBoxLayout()
        
        # Status label
        self.status_label = QLabel("Status: Starting...")
        left_panel.addWidget(self.status_label)
        
        # === AUDIO SETTINGS ===
        if AUDIO_AVAILABLE:
            audio_group = QGroupBox("Audio Settings")
            audio_layout = QVBoxLayout()
            
            # Microphone checkbox and dropdown
            mic_layout = QHBoxLayout()
            self.mic_checkbox = QCheckBox("Record Microphone")
            self.mic_checkbox.stateChanged.connect(self.toggle_microphone)
            mic_layout.addWidget(self.mic_checkbox)
            audio_layout.addLayout(mic_layout)
            
            self.mic_combo = QComboBox()
            self.mic_combo.currentIndexChanged.connect(self.on_mic_device_changed)
            audio_layout.addWidget(self.mic_combo)
            
            # Desktop audio checkbox and dropdown
            desktop_layout = QHBoxLayout()
            self.desktop_checkbox = QCheckBox("Record Desktop Audio")
            self.desktop_checkbox.stateChanged.connect(self.toggle_desktop_audio)
            desktop_layout.addWidget(self.desktop_checkbox)
            audio_layout.addLayout(desktop_layout)
            
            self.desktop_combo = QComboBox()
            self.desktop_combo.currentIndexChanged.connect(self.on_desktop_device_changed)
            audio_layout.addWidget(self.desktop_combo)
            
            # macOS-specific BlackHole instructions
            if platform.system() == "Darwin":
                blackhole_label = QLabel(
                    "⚠️ macOS: Desktop audio requires BlackHole\n"
                    "Install: brew install blackhole-2ch"
                )
                blackhole_label.setStyleSheet("color: orange; font-size: 10px;")
                blackhole_label.setWordWrap(True)
                audio_layout.addWidget(blackhole_label)
            
            audio_group.setLayout(audio_layout)
            left_panel.addWidget(audio_group)
        else:
            # Show warning if audio libraries not installed
            warning_label = QLabel("⚠️ Audio disabled: Install sounddevice and soundfile")
            warning_label.setStyleSheet("color: orange;")
            left_panel.addWidget(warning_label)
        
        # === CLIP DURATION SELECTOR ===
        duration_layout = QHBoxLayout()
        duration_layout.addWidget(QLabel("Clip Duration:"))
        
        self.duration_combo = QComboBox()
        self.duration_combo.addItem("15 seconds", 15)
        self.duration_combo.addItem("30 seconds", 30)
        self.duration_combo.addItem("1 minute", 60)
        self.duration_combo.addItem("2 minutes", 120)
        self.duration_combo.addItem("Custom...", -1)
        self.duration_combo.setCurrentIndex(1)  # Default to 30 seconds
        self.duration_combo.currentIndexChanged.connect(self.on_duration_changed)
        duration_layout.addWidget(self.duration_combo)
        left_panel.addLayout(duration_layout)
        
        # === CONTROL BUTTONS ===
        self.save_btn = QPushButton(f"Save Clip ({self.current_hotkey})")
        self.save_btn.clicked.connect(self.save_clip)
        left_panel.addWidget(self.save_btn)
        
        self.hotkey_btn = QPushButton(f"Customize Hotkey ({self.current_hotkey})")
        self.hotkey_btn.clicked.connect(self.customize_hotkey)
        left_panel.addWidget(self.hotkey_btn)
        
        # === CLIPS LIST ===
        left_panel.addWidget(QLabel("Saved Clips:"))
        self.clips_list = QListWidget()
        self.clips_list.itemClicked.connect(self.on_clip_selected)
        left_panel.addWidget(self.clips_list)
        
        # === CLIP ACTIONS ===
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
        
        # Open clips folder button
        self.folder_btn = QPushButton("Open Clips Folder")
        self.folder_btn.clicked.connect(self.open_clips_folder)
        left_panel.addWidget(self.folder_btn)
        
        main_layout.addLayout(left_panel, 1)
        
        # ===== RIGHT PANEL (Video Viewer) =====
        self.viewer = ClipViewer()
        main_layout.addWidget(self.viewer, 2)
        
        central.setLayout(main_layout)
    
    def on_duration_changed(self, index):
        """
        Handle clip duration dropdown change.
        Opens custom duration dialog if "Custom..." is selected.
        """
        duration = self.duration_combo.currentData()
        
        if duration == -1:  # Custom option selected
            dialog = DurationDialog(self)
            if dialog.exec() == QDialog.DialogCode.Accepted:
                custom_duration = dialog.get_duration()
                if custom_duration > 0:
                    # Update recorder buffer size
                    self.recorder.buffer_seconds = custom_duration
                    self.recorder.frame_buffer = deque(maxlen=custom_duration * self.recorder.fps)
                    self.update_status(f"Buffer set to {custom_duration} seconds")
                else:
                    # Invalid duration - revert to previous selection
                    self.duration_combo.setCurrentIndex(1)  # 30 seconds
                    self.show_error("Duration must be greater than 0")
            else:
                # User cancelled - revert to previous selection
                self.duration_combo.setCurrentIndex(1)  # 30 seconds
        else:
            # Update recorder buffer size
            self.recorder.buffer_seconds = duration
            self.recorder.frame_buffer = deque(maxlen=duration * self.recorder.fps)
            self.update_status(f"Buffer set to {duration} seconds")
    
    def customize_hotkey(self):
        """Open dialog to customize the hotkey."""
        if not PYNPUT_AVAILABLE:
            self.show_error(
                "Hotkey customization requires pynput library.\n\n"
                "Install with: pip install pynput"
            )
            return
        
        dialog = HotkeyDialog(self.current_hotkey, self)
        if dialog.exec() == QDialog.DialogCode.Accepted:
            new_hotkey = dialog.get_hotkey()
            if new_hotkey != self.current_hotkey:
                self.current_hotkey = new_hotkey
                
                # Restart hotkey listener with new hotkey
                self.stop_hotkey_listener()
                self.start_hotkey_listener()
                
                # Update button text
                self.save_btn.setText(f"Save Clip ({self.current_hotkey})")
                self.hotkey_btn.setText(f"Customize Hotkey ({self.current_hotkey})")
                self.update_status(f"Hotkey changed to {self.current_hotkey}")
    
    def start_hotkey_listener(self):
        """Start the global hotkey listener."""
        if not PYNPUT_AVAILABLE:
            self.update_status("Hotkeys unavailable (install pynput)")
            return
        
        try:
            self.hotkey_listener = HotkeyListener(self.current_hotkey)
            self.hotkey_listener.hotkey_pressed.connect(self.save_clip)
            self.hotkey_listener.start()
            self.update_status(f"Ready! Press {self.current_hotkey} to save clips")
        except Exception as e:
            self.show_error(f"Failed to start hotkey listener: {str(e)}")
    
    def stop_hotkey_listener(self):
        """Stop the global hotkey listener."""
        if self.hotkey_listener:
            self.hotkey_listener.stop()
            self.hotkey_listener.wait()
            self.hotkey_listener = None
    
    def load_audio_devices(self):
        """Load and populate audio device dropdowns."""
        if not AUDIO_AVAILABLE or not self.recorder.audio_recorder:
            return
        
        input_devices, output_devices = self.recorder.audio_recorder.get_audio_devices()
        
        # === POPULATE MICROPHONE DROPDOWN ===
        self.mic_combo.clear()
        for device in input_devices:
            self.mic_combo.addItem(device['name'], device['index'])
        
        # === POPULATE DESKTOP AUDIO DROPDOWN ===
        self.desktop_combo.clear()
        
        system = platform.system()
        
        if system == "Darwin":  # macOS
            # On macOS, look for BlackHole or similar virtual audio devices
            found_blackhole = False
            for device in input_devices:
                device_name_lower = device['name'].lower()
                if 'blackhole' in device_name_lower or 'soundflower' in device_name_lower:
                    self.desktop_combo.addItem(device['name'], device['index'])
                    found_blackhole = True
            
            if not found_blackhole:
                self.desktop_combo.addItem("(Install BlackHole first)", -1)
                self.desktop_checkbox.setEnabled(False)
        
        elif system == "Windows":
            # On Windows, look for Stereo Mix or Wave Out Mix
            for device in input_devices:
                device_name_lower = device['name'].lower()
                if 'stereo mix' in device_name_lower or 'wave out' in device_name_lower:
                    self.desktop_combo.addItem(device['name'], device['index'])
        
        elif system == "Linux":
            # On Linux with PulseAudio, look for monitor devices
            for device in input_devices:
                device_name_lower = device['name'].lower()
                if 'monitor' in device_name_lower or 'output' in device_name_lower:
                    self.desktop_combo.addItem(device['name'], device['index'])
        
        # If no desktop audio devices found
        if self.desktop_combo.count() == 0:
            self.desktop_combo.addItem("No desktop audio device found", -1)
            self.desktop_checkbox.setEnabled(False)
    
    def toggle_microphone(self, state):
        """
        Enable or disable microphone recording.
        
        Args:
            state: Qt.CheckState value
        """
        if not AUDIO_AVAILABLE or not self.recorder.audio_recorder:
            return
        
        enabled = state == Qt.CheckState.Checked.value
        self.recorder.audio_recorder.mic_enabled = enabled
        
        if enabled:
            device_index = self.mic_combo.currentData()
            if device_index is not None and device_index >= 0:
                self.recorder.audio_recorder.mic_device = device_index
                self.update_status("Microphone enabled")
                
                # Restart audio recording if screen recording is active
                if self.recorder.is_recording:
                    self.recorder.audio_recorder.stop_recording()
                    self.recorder.audio_recorder.start_recording()
            else:
                self.mic_checkbox.setChecked(False)
                self.show_error("Please select a microphone device")
        else:
            self.update_status("Microphone disabled")
            # Stop audio if no sources enabled
            if self.recorder.is_recording and not self.recorder.audio_recorder.desktop_enabled:
                self.recorder.audio_recorder.stop_recording()
    
    def toggle_desktop_audio(self, state):
        """
        Enable or disable desktop audio recording.
        
        Args:
            state: Qt.CheckState value
        """
        if not AUDIO_AVAILABLE or not self.recorder.audio_recorder:
            return
        
        enabled = state == Qt.CheckState.Checked.value
        self.recorder.audio_recorder.desktop_enabled = enabled
        
        if enabled:
            device_index = self.desktop_combo.currentData()
            if device_index is not None and device_index >= 0:
                self.recorder.audio_recorder.desktop_device = device_index
                self.update_status("Desktop audio enabled")
                
                # Restart audio recording if screen recording is active
                if self.recorder.is_recording:
                    self.recorder.audio_recorder.stop_recording()
                    self.recorder.audio_recorder.start_recording()
            else:
                self.desktop_checkbox.setChecked(False)
                
                # Show platform-specific instructions
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
                        "1. Install: brew install blackhole-2ch\n"
                        "2. Open Audio MIDI Setup (in Applications/Utilities)\n"
                        "3. Create a Multi-Output Device with BlackHole and your speakers\n"
                        "4. Set this as your default output\n"
                        "5. Restart this application\n"
                        "6. Select BlackHole as desktop audio source"
                    )
        else:
            self.update_status("Desktop audio disabled")
            # Stop audio if no sources enabled
            if self.recorder.is_recording and not self.recorder.audio_recorder.mic_enabled:
                self.recorder.audio_recorder.stop_recording()
    
    def on_mic_device_changed(self, index):
        """Handle microphone device selection change."""
        if not AUDIO_AVAILABLE or not self.recorder.audio_recorder:
            return
        
        device_index = self.mic_combo.currentData()
        if device_index is not None and device_index >= 0:
            self.recorder.audio_recorder.mic_device = device_index
            # Restart audio if currently recording with mic enabled
            if self.mic_checkbox.isChecked() and self.recorder.is_recording:
                self.recorder.audio_recorder.stop_recording()
                self.recorder.audio_recorder.start_recording()
    
    def on_desktop_device_changed(self, index):
        """Handle desktop audio device selection change."""
        if not AUDIO_AVAILABLE or not self.recorder.audio_recorder:
            return
        
        device_index = self.desktop_combo.currentData()
        if device_index is not None and device_index >= 0:
            self.recorder.audio_recorder.desktop_device = device_index
            # Restart audio if currently recording with desktop audio enabled
            if self.desktop_checkbox.isChecked() and self.recorder.is_recording:
                self.recorder.audio_recorder.stop_recording()
                self.recorder.audio_recorder.start_recording()
    
    def setup_signals(self):
        """Connect recorder signals to UI slots."""
        self.recorder.signals.clip_saved.connect(self.on_clip_saved)
        self.recorder.signals.status_update.connect(self.update_status)
        self.recorder.signals.error_occurred.connect(self.show_error)
        self.recorder.signals.permission_needed.connect(self.show_permission_dialog)
    
    def show_permission_dialog(self):
        """Show dialog explaining screen recording permission requirements (macOS)."""
        msg = QMessageBox(self)
        msg.setWindowTitle("Screen Recording Permission Required")
        msg.setIcon(QMessageBox.Icon.Information)
        msg.setText(
            "<b>Screen Recording Permission Needed</b><br><br>"
            "This app needs permission to record your screen.<br><br>"
            "<b>To grant permission:</b><br>"
            "1. Quit this application completely (Cmd+Q)<br>"
            "2. Open <b>System Settings</b><br>"
            "3. Go to <b>Privacy & Security</b> → <b>Screen Recording</b><br>"
            "4. Enable the checkbox for this app<br>"
            "5. Restart this application<br>"
        )
        
        msg.exec()
        self.update_status("Waiting for screen recording permission...")
    
    def auto_start_recording(self):
        """Automatically start screen recording on app launch."""
        if not self.recorder.is_recording:
            # On macOS, check if permission was denied
            if platform.system() == "Darwin":
                if hasattr(self.recorder, 'permission_granted') and not self.recorder.permission_granted:
                    return
            
            # Start recording
            success = self.recorder.start_recording()
            if success:
                self.update_status("Recording active (auto-started)")
    
    def save_clip(self):
        """Save a clip from the buffer (called by hotkey or button)."""
        # Get selected duration
        duration = self.duration_combo.currentData()
        if duration == -1:  # Custom duration
            duration = self.recorder.buffer_seconds
        
        filename = self.recorder.save_clip(duration)
        if filename:
            self.update_status("Saving clip...")
    
    def rename_clip(self):
        """Rename the selected clip."""
        current_item = self.clips_list.currentItem()
        if not current_item:
            QMessageBox.information(self, "No Clip Selected", 
                                   "Please select a clip to rename.")
            return
        
        old_filename = current_item.data(Qt.ItemDataRole.UserRole)
        old_path = self.recorder.clips_dir / old_filename
        meta_file = old_path.with_suffix('.meta')
        
        # Load current custom name from metadata file
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
        
        # Show input dialog
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
                # Load timestamp from existing metadata
                timestamp = ""
                if meta_file.exists():
                    with open(meta_file, 'r') as f:
                        lines = f.readlines()
                        for line in lines:
                            if line.startswith('timestamp='):
                                timestamp = line.split('=', 1)[1].strip()
                                break
                
                # Write updated metadata
                with open(meta_file, 'w') as f:
                    f.write(f"timestamp={timestamp}\n")
                    f.write(f"name={new_name}\n")
                
                # Refresh clips list
                self.load_clips_list()
                
                # Reselect the renamed clip
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
        """Open trim dialog for the selected clip."""
        current_item = self.clips_list.currentItem()
        if not current_item:
            QMessageBox.information(self, "No Clip Selected", 
                                   "Please select a clip to trim.")
            return
        
        filename = current_item.data(Qt.ItemDataRole.UserRole)
        clip_path = self.recorder.clips_dir / filename
        
        # Release clip in viewer if it's the same one we're trimming
        if self.viewer.current_clip_path == str(clip_path):
            self.viewer.release_current_clip()
        
        # Show trim dialog
        dialog = TrimDialog(str(clip_path), self)
        
        if dialog.exec() == QDialog.DialogCode.Accepted:
            start_frame, end_frame = dialog.get_trim_range()
            self.create_trimmed_video(str(clip_path), start_frame, end_frame)
    
    def create_trimmed_video(self, source_path, start_frame, end_frame):
        """
        Create a new trimmed video from a source clip.
        
        Args:
            source_path: Path to source video
            start_frame: First frame to include
            end_frame: Last frame to include
        """
        try:
            self.update_status("Creating trimmed clip...")
            
            # Open source video
            cap = cv2.VideoCapture(source_path)
            fps = cap.get(cv2.CAP_PROP_FPS)
            width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
            height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
            
            # Generate output filename
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            readable_time = datetime.now().strftime("%Y-%m-%d %I:%M:%S %p")
            output_path = self.recorder.clips_dir / f"clip_{timestamp}_trimmed.mp4"
            meta_path = output_path.with_suffix('.meta')
            
            # Create video writer
            fourcc = cv2.VideoWriter_fourcc(*'mp4v')
            out = cv2.VideoWriter(str(output_path), fourcc, fps, (width, height))
            
            # Seek to start frame
            cap.set(cv2.CAP_PROP_POS_FRAMES, start_frame)
            
            # Write frames from start to end
            for frame_num in range(start_frame, end_frame + 1):
                ret, frame = cap.read()
                if not ret:
                    break
                out.write(frame)
            
            cap.release()
            out.release()
            
            # Write metadata
            with open(meta_path, 'w') as f:
                f.write(f"timestamp={readable_time}\n")
                f.write(f"name=(Trimmed)\n")
            
            self.update_status(f"Trimmed clip saved")
            self.load_clips_list()
            
            # Select and load the new trimmed clip
            for i in range(self.clips_list.count()):
                item = self.clips_list.item(i)
                if item.data(Qt.ItemDataRole.UserRole) == output_path.name:
                    self.clips_list.setCurrentRow(i)
                    self.viewer.load_clip(str(output_path))
                    break
            
        except Exception as e:
            self.show_error(f"Failed to trim clip: {str(e)}")
    
    def load_clips_list(self):
        """Load and display all saved clips in the list widget."""
        self.clips_list.clear()
        
        # Get all MP4 files in clips directory, sorted by newest first
        clips = sorted(self.recorder.clips_dir.glob("*.mp4"), reverse=True)
        
        for clip in clips:
            meta_file = clip.with_suffix('.meta')
            display_name = clip.stem  # Default to filename
            
            # Load custom name and timestamp from metadata file
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
                        
                        # Format display name
                        if custom_name:
                            display_name = f"{custom_name} ({timestamp})"
                        elif timestamp:
                            display_name = f"Clip - {timestamp}"
                except:
                    pass
            
            # Add to list widget
            from PyQt6.QtWidgets import QListWidgetItem
            item = QListWidgetItem(display_name)
            item.setData(Qt.ItemDataRole.UserRole, clip.name)  # Store actual filename
            self.clips_list.addItem(item)
    
    def on_clip_selected(self, item):
        """Handle clip selection in the list."""
        filename = item.data(Qt.ItemDataRole.UserRole)
        clip_path = self.recorder.clips_dir / filename
        self.viewer.load_clip(str(clip_path))
    
    def on_clip_saved(self, filename):
        """Handle clip saved signal - refresh the clips list."""
        self.load_clips_list()
    
    def delete_clip(self):
        """Delete the selected clip."""
        current_item = self.clips_list.currentItem()
        if not current_item:
            return
        
        filename = current_item.data(Qt.ItemDataRole.UserRole)
        display_name = current_item.text()
        
        # Confirm deletion
        reply = QMessageBox.question(self, 'Delete Clip',
                                    f'Delete "{display_name}"?',
                                    QMessageBox.StandardButton.Yes | 
                                    QMessageBox.StandardButton.No)
        
        if reply == QMessageBox.StandardButton.Yes:
            clip_path = self.recorder.clips_dir / filename
            meta_path = clip_path.with_suffix('.meta')
            
            # Release clip in viewer if it's the one being deleted
            if self.viewer.current_clip_path == str(clip_path):
                self.viewer.release_current_clip()
            
            # Try multiple times to delete (file might be locked)
            max_attempts = 5
            for attempt in range(max_attempts):
                try:
                    import gc
                    gc.collect()  # Force garbage collection
                    
                    time.sleep(0.2 * (attempt + 1))  # Wait longer each attempt
                    
                    # Delete video file
                    if clip_path.exists():
                        clip_path.unlink()
                    
                    # Delete metadata file
                    if meta_path.exists():
                        meta_path.unlink()
                    
                    # Success - refresh list
                    self.load_clips_list()
                    self.update_status(f"Deleted {display_name}")
                    return
                    
                except PermissionError as e:
                    if attempt < max_attempts - 1:
                        continue  # Try again
                    else:
                        self.show_error(f"Failed to delete after {max_attempts} attempts.\n\n"
                                      f"The file is still being used.")
                except Exception as e:
                    self.show_error(f"Failed to delete: {str(e)}")
                    return
    
    def open_clips_folder(self):
        """Open the clips folder in the system file manager."""
        import subprocess
        
        folder = str(self.recorder.clips_dir)
        
        # Platform-specific commands to open folder
        if platform.system() == "Windows":
            subprocess.run(["explorer", folder])
        elif platform.system() == "Darwin":
            subprocess.run(["open", folder])
        else:  # Linux
            subprocess.run(["xdg-open", folder])
    
    def update_status(self, message):
        """
        Update the status label.
        
        Args:
            message: Status message to display
        """
        self.status_label.setText(f"Status: {message}")
    
    def show_error(self, message):
        """
        Show an error dialog.
        
        Args:
            message: Error message to display
        """
        QMessageBox.critical(self, "Error", message)
    
    def closeEvent(self, event):
        """Handle window close event - clean up resources."""
        # Release viewer resources
        self.viewer.release_current_clip()
        
        # Stop recording
        self.recorder.stop_recording()
        
        # Stop hotkey listener
        self.stop_hotkey_listener()
        
        event.accept()

def main():
    """Main entry point for the application."""
    app = QApplication(sys.argv)
    
    # Show warning if running on macOS
    if platform.system() == "Darwin":
        print("\n" + "="*60)
        print("macOS Screen Recorder")
        print("="*60)
        print("\nFor desktop audio recording:")
        print("1. Install BlackHole: brew install blackhole-2ch")
        print("2. Configure Audio MIDI Setup")
        print("3. Select BlackHole in the app's audio settings")
        print("\n" + "="*60 + "\n")
    
    # Create and show main window
    window = MainWindow()
    window.show()
    
    # Run application
    sys.exit(app.exec())

if __name__ == "__main__":
    main()
