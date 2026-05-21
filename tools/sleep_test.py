#!/usr/bin/env python3
"""
sleep_test.py - Exercise the new deep-sleep / wake feature.

Flow:
  1. Wait for boot.
  2. Send `sleep` over serial to trigger sleep mode (same code path as
     holding the encoder for >=2 s).
  3. Watch for the `[Sleep] LCD off, LED off, AI rail gated` line.
  4. Wait a few seconds with the device asleep.
  5. Send any serial command -> firmware short-circuits into exitSleepMode().
  6. Watch for `[Sleep] Awake. Back to IDLE.`
  7. PASS if both lines appeared.

Usage:  python tools/sleep_test.py [COM3] [.pio/sleep_test.log]
"""
import sys, time, threading
from pathlib import Path
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
LOG  = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(".pio/sleep_test.log")
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

ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.rts = False
time.sleep(0.05)
ser.reset_input_buffer()

stop = False
boot_done       = False
sleep_entered   = False
sleep_woke      = False
serial_wake     = False

def reader():
    global boot_done, sleep_entered, sleep_woke, serial_wake
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
            if "[Main] Ready. State: IDLE" in line:
                boot_done = True
            if "LCD off, LED off, AI rail gated" in line:
                sleep_entered = True
            if "Serial activity" in line:
                serial_wake = True
            if "Awake. Back to IDLE" in line:
                sleep_woke = True

t = threading.Thread(target=reader, daemon=True)
t.start()

say("[HOST] === DEEP-SLEEP TEST ===")
say(f"[HOST] port={PORT} log={LOG}")
say("[HOST] waiting for boot...")
deadline = time.time() + 8
while time.time() < deadline and not boot_done:
    time.sleep(0.1)

# 1. Trigger sleep via serial command.
say("\n[HOST] >>> sleep")
ser.write(b"sleep\n")
deadline = time.time() + 4
while time.time() < deadline and not sleep_entered:
    time.sleep(0.1)
if not sleep_entered:
    say("[HOST] FAIL: never saw 'LCD off' line within 4s")
else:
    say("[HOST] OK: device confirmed sleep entry.")

# 2. Hold sleep for 3 s to be sure the loop is short-circuiting.
say("\n[HOST] (sleeping for 3 s ...)")
time.sleep(3.0)

# 3. Wake via serial command.
say("\n[HOST] >>> ping (any cmd)")
ser.write(b"help\n")
deadline = time.time() + 5
while time.time() < deadline and not sleep_woke:
    time.sleep(0.1)

ser.write(b"\n")
time.sleep(1.0)

stop = True
ser.close()

say("\n[HOST] ============= RESULT =============")
say(f"[HOST] boot_done     : {boot_done}")
say(f"[HOST] sleep_entered : {sleep_entered}")
say(f"[HOST] serial_wake   : {serial_wake}")
say(f"[HOST] sleep_woke    : {sleep_woke}")

if boot_done and sleep_entered and sleep_woke:
    say("[HOST] PASS: deep-sleep enter + serial-wake roundtrip works.")
elif boot_done and sleep_entered:
    say("[HOST] PARTIAL: entered sleep but didn't wake within 5s. "
        "(Check that loop() short-circuit handles serial input.)")
else:
    say("[HOST] FAIL: see log above for evidence.")

f.close()
print(f"\n[done] log -> {LOG}")
