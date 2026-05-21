"""
USB serial bridge for communicating with the SenseCAP Watcher device.
Runs entirely on a background thread — never blocks the UI.

Protocol (newline-terminated text):
  APP → DEVICE:  PING | GET_STATUS | GET_PHOTOS | SEND_CONFIG <json>
  DEVICE → APP:  PONG | STATUS <json> | PHOTO_START <name> <bytes>
                 PHOTO_DATA <base64> | PHOTO_END | CONFIG_ACK | ERROR <msg>
"""

from __future__ import annotations
import json
import queue
import threading
import time
import base64
import random
from typing import Optional, Callable, Any

try:
    import serial
    import serial.tools.list_ports
    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False

# ── Demo mock data ──────────────────────────────────────────────────────────

DEMO_STATUS = {
    "vitality": 78,
    "streak": 7,
    "connected_at": "COM-DEMO",
    "habits": [
        {"id": "hydrate",  "name": "Hydrate",   "emoji": "💧", "color": "#A7C8F2", "completed": 5, "goal": 8},
        {"id": "read",     "name": "Read",       "emoji": "📖", "color": "#D4C1F2", "completed": 1, "goal": 2},
        {"id": "meditate", "name": "Meditate",   "emoji": "🧘", "color": "#A7F2D4", "completed": 0, "goal": 1},
        {"id": "walk",     "name": "Walk",       "emoji": "🚶", "color": "#F2C9A7", "completed": 1, "goal": 1},
    ],
    "feed": [
        {"ts": "Today 2:34pm", "habit": "Hydrate",  "color": "#A7C8F2", "log": "5/8"},
        {"ts": "Today 1:15pm", "habit": "Read",     "color": "#D4C1F2", "log": "1/2"},
        {"ts": "Today 9:00am", "habit": "Walk",     "color": "#F2C9A7", "log": "1/1"},
        {"ts": "Yesterday",    "habit": "Meditate", "color": "#A7F2D4", "log": "1/1"},
        {"ts": "Yesterday",    "habit": "Hydrate",  "color": "#A7C8F2", "log": "8/8"},
    ],
}

DEMO_PHOTOS = [
    {"name": "hydrate_001.jpg", "habit": "Hydrate", "color": "#A7C8F2", "ts": "Today 2:34pm"},
    {"name": "walk_001.jpg",    "habit": "Walk",     "color": "#F2C9A7", "ts": "Today 9:00am"},
    {"name": "read_001.jpg",    "habit": "Read",     "color": "#D4C1F2", "ts": "Yesterday 8pm"},
    {"name": "hydrate_002.jpg", "habit": "Hydrate",  "color": "#A7C8F2", "ts": "Yesterday 3pm"},
    {"name": "meditate_001.jpg","habit": "Meditate", "color": "#A7F2D4", "ts": "2 days ago"},
    {"name": "walk_002.jpg",    "habit": "Walk",     "color": "#F2C9A7", "ts": "2 days ago"},
]


class SerialBridge:
    """
    Manages USB serial communication with the Watcher device.
    All I/O happens on a daemon thread; results are posted to `_out_queue`
    which the UI drains on its own timer.
    """

    def __init__(self, demo_mode: bool = False) -> None:
        self.demo_mode = demo_mode
        self.connected = False
        self.port: Optional[str] = None

        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._serial: Optional[Any] = None

        # Commands from UI → device
        self._cmd_queue: queue.Queue = queue.Queue()
        # Events from device → UI
        self._out_queue: queue.Queue = queue.Queue()

        # Current device state (set by background thread, read by UI)
        self.status: dict = {}
        self.photos: list[dict] = []

        # Registered event listeners: event_type → list[callable]
        self._listeners: dict[str, list[Callable]] = {}

    # ── Public API (called from UI thread) ─────────────────────────────────

    def start(self) -> None:
        self._running = True
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._running = False

    def send_command(self, cmd: str, payload: str = "") -> None:
        """Queue a command to be sent to the device."""
        self._cmd_queue.put((cmd, payload))

    def ping(self) -> None:
        self.send_command("PING")

    def get_status(self) -> None:
        self.send_command("GET_STATUS")

    def get_photos(self) -> None:
        self.send_command("GET_PHOTOS")

    def send_config(self, config_json: str) -> None:
        self.send_command("SEND_CONFIG", config_json)

    def on(self, event: str, callback: Callable) -> None:
        """Register a listener for a device event (called from UI thread)."""
        self._listeners.setdefault(event, []).append(callback)

    def process_queue(self, app: Any) -> None:
        """
        Drain the output queue and fire registered listeners.
        Call this periodically from the UI thread (e.g., every 200ms via after()).
        """
        while not self._out_queue.empty():
            try:
                event, data = self._out_queue.get_nowait()
                for cb in self._listeners.get(event, []):
                    try:
                        cb(data)
                    except Exception:
                        pass
            except queue.Empty:
                break

    # ── Background thread ───────────────────────────────────────────────────

    def _run(self) -> None:
        if self.demo_mode:
            self._run_demo()
        else:
            self._run_serial()

    # ── Demo mode ───────────────────────────────────────────────────────────

    def _run_demo(self) -> None:
        """Simulate a connected device with mock data and live vitality drift."""
        time.sleep(0.4)  # brief startup delay to let UI settle
        self.connected = True
        self.port = "DEMO"
        self.status = dict(DEMO_STATUS)
        self.photos = list(DEMO_PHOTOS)
        self._emit("connected", "DEMO")
        self._emit("status", dict(self.status))

        tick = 0
        while self._running:
            # Process outgoing commands
            while not self._cmd_queue.empty():
                try:
                    cmd, payload = self._cmd_queue.get_nowait()
                    self._handle_demo_command(cmd, payload)
                except queue.Empty:
                    break

            # Emit status every ~5 s (25 × 200 ms ticks)
            tick += 1
            if tick >= 25:
                tick = 0
                # Drift vitality slightly so the ring animates
                self.status["vitality"] = max(
                    10, min(100, self.status["vitality"] + random.randint(-3, 2))
                )
                self._emit("status", dict(self.status))

            time.sleep(0.2)

    def _handle_demo_command(self, cmd: str, payload: str) -> None:
        if cmd == "PING":
            self._emit("pong", None)
        elif cmd == "GET_STATUS":
            self._emit("status", dict(self.status))
        elif cmd == "GET_PHOTOS":
            self._emit("photo_list", list(self.photos))
        elif cmd == "SEND_CONFIG":
            time.sleep(0.3)  # simulate transfer
            self._emit("config_ack", None)

    # ── Real serial mode ────────────────────────────────────────────────────

    def _run_serial(self) -> None:
        if not SERIAL_AVAILABLE:
            self._emit("error", "pyserial not installed")
            return

        while self._running:
            # Auto-discover port
            port = self._find_device_port()
            if not port:
                time.sleep(2)
                continue

            try:
                self._serial = serial.Serial(port, baudrate=115200, timeout=1)
                self.connected = True
                self.port = port
                self._emit("connected", port)

                status_timer = 0.0
                while self._running:
                    # Send periodic GET_STATUS
                    status_timer += 0.2
                    if status_timer >= 5.0:
                        status_timer = 0.0
                        self._serial.write(b"GET_STATUS\n")

                    # Process queued commands
                    while not self._cmd_queue.empty():
                        try:
                            cmd, payload = self._cmd_queue.get_nowait()
                            line = f"{cmd} {payload}\n".strip() + "\n"
                            self._serial.write(line.encode())
                        except queue.Empty:
                            break

                    # Read incoming lines
                    if self._serial.in_waiting:
                        raw = self._serial.readline().decode("utf-8", errors="ignore").strip()
                        if raw:
                            self._parse_device_line(raw)

                    time.sleep(0.2)

            except Exception as e:
                self._emit("error", str(e))
            finally:
                if self._serial:
                    try:
                        self._serial.close()
                    except Exception:
                        pass
                self.connected = False
                self.port = None
                self._emit("disconnected", None)
                time.sleep(2)

    def _find_device_port(self) -> Optional[str]:
        """Detect Watcher over USB serial (CH340/CP210x VID)."""
        if not SERIAL_AVAILABLE:
            return None
        WATCHER_VIDS = {0x10C4, 0x1A86}  # CP210x, CH340
        for p in serial.tools.list_ports.comports():
            vid = getattr(p, "vid", None)
            if vid and vid in WATCHER_VIDS:
                return p.device
        return None

    def _parse_device_line(self, line: str) -> None:
        if line == "PONG":
            self._emit("pong", None)
        elif line == "CONFIG_ACK":
            self._emit("config_ack", None)
        elif line.startswith("STATUS "):
            try:
                self.status = json.loads(line[7:])
                self._emit("status", dict(self.status))
            except json.JSONDecodeError:
                pass
        elif line.startswith("ERROR "):
            self._emit("error", line[6:])
        elif line.startswith("PHOTO_START "):
            parts = line.split()
            if len(parts) >= 3:
                self._emit("photo_start", {"name": parts[1], "size": int(parts[2])})
        elif line.startswith("PHOTO_DATA "):
            chunk = base64.b64decode(line[11:])
            self._emit("photo_data", chunk)
        elif line == "PHOTO_END":
            self._emit("photo_end", None)

    def _emit(self, event: str, data: Any) -> None:
        self._out_queue.put((event, data))
