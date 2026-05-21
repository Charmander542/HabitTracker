#!/usr/bin/env python3
"""
interactive_knob_test.py — reset the device, flip it into knob-live mode,
then stream serial output for 30 s while the user physically turns the knob
and clicks it. Captures a tagged log so we can correlate A/B toggles and
button edges with the user actions.

Usage:
    python tools/interactive_knob_test.py COM3 .pio/knob_interactive.log

During the 30 s window: turn the knob both directions, then click it several
times. Watch the terminal for lines like:
    A=1 B=0 btn=up  accumTotal=+3
    [btn] PRESS
"""
import sys, time, threading
from pathlib import Path

try:
    import serial
except ImportError:
    sys.exit("install pyserial: pip install pyserial")

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
LOG  = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(".pio/knob_interactive.log")
BAUD = 115200

LOG.parent.mkdir(parents=True, exist_ok=True)
logf = open(LOG, "w", encoding="utf-8", errors="replace")

def out(line: str):
    print(line)
    logf.write(line + "\n")
    logf.flush()

out(f"[HOST] opening {PORT} @ {BAUD}")
ser = serial.Serial()
ser.port     = PORT
ser.baudrate = BAUD
ser.dtr      = False
ser.rts      = False
ser.timeout  = 0.05
ser.open()

time.sleep(0.2)
ser.dtr = True;  ser.rts = True
time.sleep(0.05)
ser.dtr = False; ser.rts = False
time.sleep(0.05)
ser.dtr = True;  ser.rts = True
time.sleep(2.0)
ser.reset_input_buffer()

stop = False

def reader():
    while not stop:
        try:
            data = ser.read(4096)
        except Exception as exc:
            out(f"[READER-ERR] {exc}")
            return
        if data:
            for line in data.decode("utf-8", errors="replace").splitlines():
                if line:
                    out(line)

t = threading.Thread(target=reader, daemon=True)
t.start()

out("[HOST] waiting 4s for setup() to finish")
time.sleep(4.0)

out("")
out("========================================================")
out(" STEP 1: about to start a 30 s KNOB LIVE DUMP")
out("         TURN THE KNOB both directions, then CLICK it.")
out("========================================================")
out("")

ser.write(b"knob\n")  # fires cmd_knob — 10 s dump
time.sleep(10.5)

out("")
out("[HOST] second 10 s knob window (turn more aggressively)")
out("")
ser.write(b"knob\n")
time.sleep(10.5)

out("")
out("========================================================")
out(" STEP 2: 10 s BUTTON LIVE DUMP — CLICK THE KNOB 5 TIMES")
out("========================================================")
out("")
ser.write(b"btn\n")
time.sleep(11.0)

out("")
out("========================================================")
out(" STEP 3: read GPIO + IO-expander state to confirm wiring")
out("========================================================")
out("")
ser.write(b"stats\n")
time.sleep(1.5)

stop = True
time.sleep(0.2)
ser.close()
logf.close()
print(f"\n[done] saved → {LOG}")
