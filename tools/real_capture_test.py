#!/usr/bin/env python3
"""
real_capture_test.py - Verify the "real image" capture flow end-to-end.

What it proves:
  1. SSCMA (Himax) presence/absence is reported on boot.
  2. Each `preview` command produces fresh procedural pixel data:
     - the "[Camera] capture #N" banner advances,
     - the "[synth] frame #N samples" line shows DIFFERENT TL/TR/BL/BR
       hex values across runs (proves the buffer is regenerated, not
       cached/static), and
     - "[GUI] blitting RGB565 bitmap 240x240" confirms the GUI actually
       draws it.
  3. Display stays pinned for ~5 s so the human can SEE the polaroid
     with the unique "#N" stamp and "SIM" badge.

Run:  python tools/real_capture_test.py [COM3] [.pio/real_capture.log]
"""
import sys, time, threading, re
from pathlib import Path
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
LOG  = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(".pio/real_capture.log")
LOG.parent.mkdir(parents=True, exist_ok=True)
f = open(LOG, "w", encoding="utf-8", errors="replace")

def say(s: str):
    print(s)
    try:
        f.write(s + "\n"); f.flush()
    except UnicodeEncodeError:
        f.write(s.encode("ascii", errors="replace").decode("ascii") + "\n"); f.flush()

ser = serial.Serial()
ser.port     = PORT
ser.baudrate = 115200
ser.dtr      = False
ser.rts      = False
ser.timeout  = 0.05
ser.open()

# Pulse RTS/DTR like a normal terminal so the chip stays in run mode.
time.sleep(0.2)
ser.dtr = True ; ser.rts = True ; time.sleep(0.05)
ser.dtr = False; ser.rts = False; time.sleep(0.05)
ser.dtr = True ; ser.rts = True

time.sleep(2.5)  # let setup() finish
ser.reset_input_buffer()

stop      = False
samples   = []        # list of (frame_idx, TL, TR, BL, BR)
sscma_alive = None    # set on the first "[Main] SSCMA camera" line

SAMPLE_RE = re.compile(
    r"\[synth\] frame #(\d+) samples: TL=0x([0-9A-Fa-f]+) "
    r"TR=0x([0-9A-Fa-f]+) BL=0x([0-9A-Fa-f]+) BR=0x([0-9A-Fa-f]+)"
)

def reader():
    global sscma_alive
    while not stop:
        try:
            d = ser.read(4096)
        except Exception:
            return
        if not d:
            continue
        for line in d.decode("utf-8", errors="replace").splitlines():
            if not line:
                continue
            say(line)
            m = SAMPLE_RE.search(line)
            if m:
                samples.append((int(m.group(1)),
                                m.group(2), m.group(3),
                                m.group(4), m.group(5)))
            if "SSCMA camera" in line:
                if "alive" in line.lower() or "ready" in line.lower():
                    sscma_alive = True
                elif "NOT" in line or "not " in line:
                    sscma_alive = False

t = threading.Thread(target=reader, daemon=True)
t.start()

say("[HOST] === REAL CAPTURE TEST ===")
say(f"[HOST] port={PORT} log={LOG}")
say("[HOST] waiting 4 s for boot to finish...")
time.sleep(4.0)

for round_i in range(3):
    say(f"\n[HOST] === capture round {round_i + 1} ===")
    say("[HOST] >>> preview")
    ser.write(b"preview\n")
    time.sleep(5.5)  # synth + draw + linger so we can see it
    say("[HOST] >>> test off")
    ser.write(b"test off\n")
    time.sleep(1.0)
    ser.write(b"state idle\n")
    time.sleep(1.5)

say("\n[HOST] === results ===")
say(f"[HOST] SSCMA alive (real camera) : {sscma_alive}")
say(f"[HOST] synth frame samples seen  : {len(samples)}")
for idx, tl, tr, bl, br in samples:
    say(f"[HOST]   frame#{idx:>3}  TL=0x{tl}  TR=0x{tr}  BL=0x{bl}  BR=0x{br}")
if len(samples) >= 2:
    distinct = len({(s[1], s[2], s[3], s[4]) for s in samples})
    say(f"[HOST] distinct sample tuples    : {distinct} of {len(samples)}")
    if distinct >= 2:
        say("[HOST] PASS: pixel data varies across captures (buffer is "
            "regenerated each time)")
    else:
        say("[HOST] WARN: pixel data identical across captures — synth seed "
            "may be deterministic")

stop = True
time.sleep(0.3)
ser.close()
f.close()
print(f"[done] log -> {LOG}")
