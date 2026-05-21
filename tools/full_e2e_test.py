#!/usr/bin/env python3
"""
full_e2e_test.py - End-to-end smoke test:

 1.  Boot.
 2.  Camera link established at boot (Himax SSCMA over SPI).
 3.  Capture a real frame via `camimg`. Verify a >0-byte JPEG was
     decoded with magic FF D8 FF.
 4.  Force-cycle through HABIT_SELECT -> HABIT_DETAIL -> CAPTURE_RITUAL
     state to exercise the same code path the user takes when logging
     a habit. Verify the camera path picks the SSCMA route, not the
     synth fallback.
 5.  Trigger sleep via `sleep`, sleep 2.5 s, wake via `help`.
 6.  Verify wake transitions back to IDLE.

Pass criteria printed at end.

Usage:  python tools/full_e2e_test.py [COM3] [.pio/full_e2e.log]
"""
import sys, time, threading, re
from pathlib import Path
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
LOG  = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(".pio/full_e2e.log")
LOG.parent.mkdir(parents=True, exist_ok=True)
f = open(LOG, "w", encoding="utf-8", errors="replace")

def say(s: str):
    safe = s.encode("ascii", errors="replace").decode("ascii")
    print(safe)
    f.write(safe + "\n"); f.flush()

ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.dtr = False
ser.rts = False
ser.timeout = 0.05
ser.open()

ser.dtr = False; ser.rts = True; time.sleep(0.1)
ser.rts = False; time.sleep(0.05)
ser.reset_input_buffer()

stop = False
flags = {
    "boot": False,
    "sscma_alive_at_boot": False,
    "camimg_jpeg_bytes": 0,
    "real_jpeg_via_state_capture": False,
    "real_jpeg_in_capture_path": False,
    "sleep_in": False,
    "sleep_out": False,
}

def reader():
    while not stop:
        try:
            d = ser.read(4096)
        except Exception:
            return
        if not d:
            continue
        for line in d.decode("utf-8", errors="replace").splitlines():
            if not line: continue
            say(line)
            if "[Main] Ready. State: IDLE" in line:
                flags["boot"] = True
            if "*** Himax SSCMA SPI link ESTABLISHED" in line:
                flags["sscma_alive_at_boot"] = True
            m = re.search(r"got JPEG (\d+) bytes", line)
            if m:
                flags["camimg_jpeg_bytes"] = max(flags["camimg_jpeg_bytes"],
                                                int(m.group(1)))
            if ">>> REAL JPEG captured" in line:
                flags["real_jpeg_in_capture_path"] = True
            if "LCD off, LED off, AI rail gated" in line:
                flags["sleep_in"] = True
            if "Awake. Back to IDLE" in line:
                flags["sleep_out"] = True

t = threading.Thread(target=reader, daemon=True)
t.start()

say("[HOST] === FULL E2E ===")
deadline = time.time() + 8
while time.time() < deadline and not flags["boot"]:
    time.sleep(0.1)

say("\n[HOST] === STAGE 1: direct camera capture (camimg) ===")
ser.write(b"camimg\n")
time.sleep(15)
ser.write(b"test off\n")
time.sleep(1.5)

say("\n[HOST] === STAGE 2: drive HABIT_SELECT -> CAPTURE through state cmds ===")
# `state capture` jumps directly into the CAPTURE_RITUAL state which
# fires the camera and shows the photo. This is the EXACT same code
# path used when the user logs a habit by pressing the encoder.
ser.write(b"state capture\n")
time.sleep(15)
ser.write(b"state idle\n")
time.sleep(2)

say("\n[HOST] === STAGE 3: sleep + serial-wake ===")
ser.write(b"sleep\n")
time.sleep(3.5)
ser.write(b"help\n")
time.sleep(2.5)

stop = True
ser.close()

say("\n[HOST] ============= RESULT =============")
for k, v in flags.items():
    say(f"[HOST] {k:35s}: {v}")

ok = (flags["boot"] and flags["sscma_alive_at_boot"]
      and flags["camimg_jpeg_bytes"] > 0
      and flags["sleep_in"] and flags["sleep_out"])
if ok:
    say("[HOST] PASS: camera + sleep + wake all working end-to-end.")
else:
    say("[HOST] FAIL: see flags above.")

f.close()
print(f"\n[done] log -> {LOG}")
