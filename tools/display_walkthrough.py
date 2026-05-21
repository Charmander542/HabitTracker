#!/usr/bin/env python3
"""
display_walkthrough.py — drive the SPD2010 panel through a timed slideshow so
the user can visually confirm what's on screen at each step.

Each pattern is held for ~4 seconds with a prompt printed to the host console.
Look at the Watcher during each step; after the run is done, report what you
actually saw at each step so we can correlate software state to pixels.

Usage:  python tools/display_walkthrough.py COM3 .pio/display_walkthrough.log
"""
import sys, time, threading
from pathlib import Path

try:
    import serial
except ImportError:
    sys.exit("install pyserial: pip install pyserial")

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
LOG  = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(".pio/display_walkthrough.log")
BAUD = 115200

LOG.parent.mkdir(parents=True, exist_ok=True)
logf = open(LOG, "w", encoding="utf-8", errors="replace")

def out(line: str):
    print(line)
    logf.write(line + "\n")
    logf.flush()

ser = serial.Serial()
ser.port = PORT; ser.baudrate = BAUD
ser.dtr = False; ser.rts = False
ser.timeout = 0.05
ser.open()

# Reset sequence so we start from a clean boot
time.sleep(0.2)
ser.dtr = True; ser.rts = True
time.sleep(0.05)
ser.dtr = False; ser.rts = False
time.sleep(0.05)
ser.dtr = True; ser.rts = True
time.sleep(2.0)
ser.reset_input_buffer()

stop = False
def reader():
    while not stop:
        try:
            data = ser.read(4096)
        except Exception:
            return
        if data:
            for line in data.decode("utf-8", errors="replace").splitlines():
                if line: out(line)

t = threading.Thread(target=reader, daemon=True); t.start()

out("[HOST] booting — waiting 4s")
time.sleep(4.0)

STEPS = [
    ("RED FULLSCREEN — expect the whole panel to glow solid red.",  "disp red",    4.0),
    ("GREEN FULLSCREEN — expect solid green.",                       "disp green",  4.0),
    ("BLUE FULLSCREEN — expect solid blue.",                         "disp blue",   4.0),
    ("WHITE FULLSCREEN — expect brightest white.",                   "disp white",  4.0),
    ("BLACK FULLSCREEN — expect the panel to go dark (backlight still on).", "disp black", 4.0),
    ("RGBW BARS — expect four horizontal stripes: red, green, blue, white (top to bottom).", "disp bars", 5.0),
    ("DUCK FRAME 0 (OPEN eyes) — expect a large pixelated duck in the centre.",   "duck 0", 4.0),
    ("DUCK FRAME 1 (CLOSED eyes) — same duck, blinking.",                         "duck 1", 4.0),
    ("DUCK FRAME 2 (SAD)    — duck with sad eyes.",                               "duck 2", 4.0),
    ("DUCK FRAME 6 (DEAD) — X-eyes variant.",                                      "duck 6", 4.0),
    ("DUCK FRAME 8 (HAPPY LYING) — duck laying down.",                             "duck 8", 4.0),
    ("RESUME APP — unpins the test overlay, returns to live IDLE screen (duck + halo + vitality ring + time).", "test off", 2.0),
    ("FORCE IDLE STATE — re-enters IDLE; watch for vitality ring around the edge.", "state idle", 3.0),
]

for i, (label, cmd, hold) in enumerate(STEPS, start=1):
    out("")
    out(f"========== STEP {i:02d}/{len(STEPS)} ==========")
    out(f"  >>> {label}")
    out(f"  sending: {cmd}")
    ser.write((cmd + "\n").encode("utf-8"))
    time.sleep(hold)

out("")
out("========== DONE — look back at what you saw at each step ==========")
stop = True; time.sleep(0.3)
ser.close(); logf.close()
print(f"\n[done] saved → {LOG}")
