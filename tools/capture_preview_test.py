#!/usr/bin/env python3
"""capture_preview_test.py — reset, wait for boot, send `preview`, capture logs."""
import sys, time, threading
from pathlib import Path
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
LOG  = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(".pio/preview_test.log")
LOG.parent.mkdir(parents=True, exist_ok=True)
f = open(LOG, "w", encoding="utf-8", errors="replace")

def say(s):
    print(s); f.write(s + "\n"); f.flush()

ser = serial.Serial()
ser.port = PORT; ser.baudrate = 115200
ser.dtr = False; ser.rts = False
ser.timeout = 0.05
ser.open()

time.sleep(0.2)
ser.dtr = True; ser.rts = True; time.sleep(0.05)
ser.dtr = False; ser.rts = False; time.sleep(0.05)
ser.dtr = True; ser.rts = True
time.sleep(2.0)
ser.reset_input_buffer()

stop = False
def reader():
    while not stop:
        try:
            d = ser.read(4096)
        except Exception: return
        if d:
            for line in d.decode("utf-8", errors="replace").splitlines():
                if line: say(line)

t = threading.Thread(target=reader, daemon=True); t.start()
say("[HOST] booting — 4 s")
time.sleep(4.0)
say("[HOST] >>> preview")
ser.write(b"preview\n")
time.sleep(5.0)   # let it draw + hold
say("[HOST] >>> test off")
ser.write(b"test off\n")
time.sleep(1.0)
say("[HOST] >>> state idle")
ser.write(b"state idle\n")
time.sleep(2.0)
stop = True; time.sleep(0.3)
ser.close(); f.close()
print(f"[done] -> {LOG}")
