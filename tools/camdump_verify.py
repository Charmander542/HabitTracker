#!/usr/bin/env python3
import base64
import re
import sys
import time
from pathlib import Path
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
LOG  = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(".pio/camdump_verify.log")
RUNS = int(sys.argv[3]) if len(sys.argv) > 3 else 3
LOG.parent.mkdir(parents=True, exist_ok=True)
f = open(LOG, "w", encoding="utf-8", errors="replace")

def say(s: str):
    s = s.encode("ascii", errors="replace").decode("ascii")
    print(s)
    f.write(s + "\n")
    f.flush()

ser = serial.Serial(PORT, 115200, timeout=0.05)
ser.dtr = False
ser.rts = False
ser.rts = True
time.sleep(0.1)
ser.rts = False
time.sleep(0.1)
ser.reset_input_buffer()

boot_deadline = time.time() + 10
boot = ""
while time.time() < boot_deadline:
    d = ser.read(4096)
    if d:
        boot += d.decode("utf-8", errors="replace")
    if "[Main] Ready. State: IDLE" in boot:
        break
say(f"[boot_ready] {'[Main] Ready. State: IDLE' in boot}")

passes = 0
crcs = []
for i in range(1, RUNS + 1):
    say(f"[run {i}] sending camdump")
    ser.write(b"camdump\n")

    collecting = False
    b64_lines = []
    header = ""
    t_end = time.time() + 30
    while time.time() < t_end:
        d = ser.read(4096)
        if not d:
            continue
        txt = d.decode("utf-8", errors="replace")
        for ln in txt.splitlines():
            say(ln)
            if ln.startswith("[camdump] OK"):
                header = ln
            if "CAMJPEG_BEGIN" in ln:
                collecting = True
                continue
            if "CAMJPEG_END" in ln and collecting:
                collecting = False
                t_end = time.time()  # done
                break
            if collecting:
                b64_lines.append(ln.strip())

    ok = False
    reason = "missing_data"
    if header and b64_lines:
        m = re.search(r"len=(\d+)\s+crc32=([0-9A-Fa-f]{8})", header)
        if m:
            exp_len = int(m.group(1))
            exp_crc = int(m.group(2), 16)
            try:
                blob = base64.b64decode("".join(b64_lines), validate=False)
                crc = 0xFFFFFFFF
                for b in blob:
                    crc ^= b
                    for _ in range(8):
                        mask = -(crc & 1) & 0xFFFFFFFF
                        crc = ((crc >> 1) ^ (0xEDB88320 & mask)) & 0xFFFFFFFF
                crc = (~crc) & 0xFFFFFFFF
                has_eoi = (blob.rfind(b"\xFF\xD9") != -1)
                if len(blob) == exp_len and crc == exp_crc and blob[:3] == b"\xFF\xD8\xFF" and has_eoi:
                    ok = True
                    reason = "ok"
                    crcs.append(exp_crc)
                else:
                    reason = f"bad_jpeg len={len(blob)}/{exp_len} crc={crc:08X}/{exp_crc:08X} eoi={has_eoi}"
            except Exception as e:
                reason = f"decode_error {e}"
        else:
            reason = "header_parse_fail"
    say(f"[run {i}] result={ok} reason={reason}")
    if ok:
        passes += 1
    time.sleep(0.3)

ser.close()
distinct = len(set(crcs))
say(f"[summary] passes={passes}/{RUNS} distinct_crc={distinct}")
if passes == RUNS:
    say("[PASS] serial photo dump appears legit")
else:
    say("[FAIL] capture stability or freshness still not good enough")
f.close()
