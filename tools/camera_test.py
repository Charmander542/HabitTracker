#!/usr/bin/env python3
"""
camera_test.py - Verify the SSCMA SPI driver after the trailer-position fix.

What it does:
  1. Resets the device, captures the boot banner.
  2. Looks for the "[sscma] *** Himax SSCMA SPI link ESTABLISHED ***" line
     (success) or the failure path with diagnostic hex tail.
  3. Issues `camstatus` and `camcold 4000` to get a forensic dump even if
     boot probing failed.
  4. Issues `camsend AT+ID?` and `camsend AT+VER?` -- success means the
     chip returned valid SSCMA JSON, definitive proof that SPI works.
  5. Issues `camimg` to attempt a real photo capture.
  6. Reports PASS / FAIL with detailed evidence.

Usage:  python tools/camera_test.py [COM3] [.pio/camera_test.log]
"""
import sys, time, threading, re
from pathlib import Path
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
LOG  = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(".pio/camera_test.log")
LOG.parent.mkdir(parents=True, exist_ok=True)
f = open(LOG, "w", encoding="utf-8", errors="replace")

def say(s: str):
    safe = s.encode("ascii", errors="replace").decode("ascii")
    print(safe)
    f.write(safe + "\n"); f.flush()

ser = serial.Serial()
ser.port     = PORT
ser.baudrate = 115200
ser.dtr      = False
ser.rts      = False
ser.timeout  = 0.05
ser.open()

# Reset the device.
ser.dtr = False  # GPIO0 stays HIGH (normal boot)
ser.rts = True   # EN LOW
time.sleep(0.1)
ser.rts = False  # EN HIGH (reset released)
time.sleep(0.05)
ser.reset_input_buffer()

stop = False
captured = []          # raw lines

# Markers we care about
boot_done       = False
sscma_link_ok   = None
sscma_id_seen   = False
sscma_ver_seen  = False
camimg_ok       = False
camimg_jpeg_len = None

def reader():
    global boot_done, sscma_link_ok, sscma_id_seen, sscma_ver_seen
    global camimg_ok, camimg_jpeg_len
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
            captured.append(line)
            say(line)
            if "[Main] Ready. State: IDLE" in line:
                boot_done = True
            if "*** Himax SSCMA SPI link ESTABLISHED" in line:
                sscma_link_ok = True
            if "SSCMA camera NOT responding" in line or "no JSON response to AT+ID?" in line:
                sscma_link_ok = False
            if '"name":"ID?"' in line or "ID?" in line and '"code":0' in line:
                sscma_id_seen = True
            m = re.search(r"got JPEG (\d+) bytes", line)
            if m:
                camimg_ok = True
                camimg_jpeg_len = int(m.group(1))

t = threading.Thread(target=reader, daemon=True)
t.start()

say("[HOST] === SSCMA SPI CAMERA TEST ===")
say(f"[HOST] port={PORT} log={LOG}")
say("[HOST] waiting up to 8 s for boot to finish...")
deadline = time.time() + 8
while time.time() < deadline and not boot_done:
    time.sleep(0.1)
if not boot_done:
    say("[HOST] WARN: did not see 'Ready. State: IDLE' within 8s. "
        "Continuing anyway.")

# --- 1. Status snapshot
say("\n[HOST] >>> camstatus")
ser.write(b"camstatus\n")
time.sleep(2.0)

# --- 2. Cold-boot probe (RST pulse + 4s observe)
say("\n[HOST] >>> camcold 4000")
ser.write(b"camcold 4000\n")
time.sleep(6.0)

# --- 3. Re-init explicitly with verbose
say("\n[HOST] >>> caminit")
ser.write(b"caminit\n")
time.sleep(5.0)

# --- 4. AT+ID? probe via dedicated command
say("\n[HOST] >>> camsend AT+ID?")
ser.write(b"camsend AT+ID?\n")
time.sleep(3.5)

# --- 5. AT+VER? for additional confidence
say("\n[HOST] >>> camsend AT+VER?")
ser.write(b"camsend AT+VER?\n")
time.sleep(3.5)

# --- 6. Real image capture attempt
say("\n[HOST] >>> camimg")
ser.write(b"camimg\n")
# Firmware has 15s capture timeout; give it room.
time.sleep(20.0)

ser.write(b"test off\n")
time.sleep(1.5)

stop = True
time.sleep(0.5)
ser.close()

# ----------------------------------------------------------------- Verdict
say("\n[HOST] ============= RESULT =============")
say(f"[HOST] boot finished        : {boot_done}")
say(f"[HOST] SSCMA link OK on boot: {sscma_link_ok}")
say(f"[HOST] AT+ID response seen  : {sscma_id_seen}")
say(f"[HOST] camimg got JPEG      : {camimg_ok} ({camimg_jpeg_len} bytes)")

# Search the captured log for byte-pattern evidence of the chip really
# responding (not just noise).
joined = "\n".join(captured)
if sscma_link_ok or camimg_ok:
    say("[HOST] PASS: Himax SPI link is alive.")
elif "0x3B" in joined or any(x in joined for x in ("D1 D1 D1", "8E 8E 8E", "FF FF FF FF FF FF")):
    say("[HOST] FAIL: still seeing repeating-byte garbage (not SSCMA framing).")
else:
    say("[HOST] FAIL: no clear SSCMA response. See log for byte-level traces.")

f.close()
print(f"\n[done] log -> {LOG}")
