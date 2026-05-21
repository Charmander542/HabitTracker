#!/usr/bin/env python3
import sys, time, threading
from pathlib import Path
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
LOG  = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(".pio/capture_stability.log")
LOG.parent.mkdir(parents=True, exist_ok=True)
f = open(LOG, "w", encoding="utf-8", errors="replace")

def say(s: str):
    s = s.encode("ascii", errors="replace").decode("ascii")
    print(s)
    f.write(s + "\n"); f.flush()

ser = serial.Serial(PORT, 115200, timeout=0.05)
ser.dtr = False
ser.rts = False
ser.rts = True
time.sleep(0.1)
ser.rts = False
time.sleep(0.05)
ser.reset_input_buffer()

stop = False
all_lines = []

def reader():
    while not stop:
        d = ser.read(4096)
        if not d:
            continue
        txt = d.decode("utf-8", errors="replace")
        for ln in txt.splitlines():
            if not ln:
                continue
            all_lines.append(ln)
            say(ln)

t = threading.Thread(target=reader, daemon=True)
t.start()

deadline = time.time() + 10
while time.time() < deadline:
    if any("[Main] Ready. State: IDLE" in ln for ln in all_lines):
        break
    time.sleep(0.1)

real_total = 0
synth_total = 0
fail_total = 0

for i in range(1, 4):
    say(f"[HOST] --- capture cycle {i} ---")
    ser.write(b"state capture\n")
    t_end = time.time() + 20
    start_idx = len(all_lines)
    while time.time() < t_end:
        window = all_lines[start_idx:]
        if any("[State] CAPTURE_RITUAL -> CELEBRATION" in ln for ln in window):
            break
        time.sleep(0.05)
    window = all_lines[start_idx:]
    r = sum(1 for ln in window if ">>> REAL JPEG captured" in ln)
    s = sum(1 for ln in window if ">>> SYNTH frame ready" in ln)
    fa = sum(1 for ln in window if "SSCMA capture FAILED" in ln)
    real_total += r
    synth_total += s
    fail_total += fa
    say(f"[HOST] cycle {i}: real={r} synth={s} fail={fa}")
    ser.write(b"state idle\n")
    time.sleep(0.25)

stop = True
time.sleep(0.2)
ser.close()

say("[HOST] === RESULT ===")
say(f"[HOST] real_total={real_total}")
say(f"[HOST] synth_total={synth_total}")
say(f"[HOST] fail_total={fail_total}")
f.close()
