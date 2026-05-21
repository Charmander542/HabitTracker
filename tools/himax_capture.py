#!/usr/bin/env python3
"""
himax_capture.py - Capture a real JPEG from the Himax HX6538 SSCMA over
its USB-CDC port (COM4 @ 921600), save it to disk, and report stats.

Background:
    The previous probe (himax_uart_probe.py) confirmed the AI chip on
    this device IS running full SSCMA firmware at 921600 baud over COM4.
    AT+ID?/NAME?/VER?/BREAK all respond with proper JSON envelopes.

What we're doing here:
    1. Get the help output so we know the exact "give me a JPEG" command
       (varies by SSCMA version: AT+SAMPLE=1, AT+INVOKE=..., AT+SNAP, etc).
    2. Try the most likely image-capture commands in order:
         AT+SAMPLE=1
         AT+INVOKE=1,0,0
         AT+ALGO?
         AT+ACTION=...
       Each command's full response is logged, and any base64-encoded
       JPEG payload is decoded to disk.
    3. If we get bytes that look like a JPEG (FF D8 FF magic), save it
       so the user can OPEN the file and SEE the camera output.
"""
import sys, time, json, base64, re, threading
from pathlib import Path
import serial

PORT      = sys.argv[1] if len(sys.argv) > 1 else "COM4"
OUT_DIR   = Path(sys.argv[2] if len(sys.argv) > 2 else ".pio/himax_captures")
OUT_DIR.mkdir(parents=True, exist_ok=True)
LOG       = OUT_DIR / "session.log"
f = open(LOG, "w", encoding="utf-8", errors="replace")

def say(s):
    print(s)
    try:
        f.write(s + "\n"); f.flush()
    except UnicodeEncodeError:
        f.write(s.encode("ascii", errors="replace").decode("ascii") + "\n"); f.flush()

# ----------------------------------------------------------------
# UART setup. SSCMA framing is "\r{...}\n" per JSON event, so we read
# until we see }\n then return everything up to that point.
# ----------------------------------------------------------------
ser = serial.Serial()
ser.port = PORT
ser.baudrate = 921600
ser.timeout = 0.1
# We learned: opening COM4 with default DTR/RTS=False lets the chip
# stay in SSCMA mode (no spurious reset).
ser.dtr = False
ser.rts = False
ser.open()
time.sleep(0.3)
ser.reset_input_buffer()

def read_for(seconds, label=""):
    """Drain the UART for `seconds` while logging all bytes."""
    end = time.time() + seconds
    buf = bytearray()
    while time.time() < end:
        chunk = ser.read(8192)
        if chunk:
            buf.extend(chunk)
            end = max(end, time.time() + 0.4)  # extend on activity
        else:
            time.sleep(0.02)
    if label and buf:
        say(f"  [{label}] {len(buf)} bytes")
    return bytes(buf)

def send_at(cmd, timeout=4.0, label=None):
    label = label or cmd.strip()
    say(f"\n>>> {label}")
    ser.reset_input_buffer()
    ser.write(cmd.encode("ascii"))
    try: ser.flush()
    except Exception: pass
    raw = read_for(timeout, label="rx")
    # SSCMA framing: \r{ ... }\n events, possibly multiple per response
    text = raw.decode("latin-1", errors="replace")
    events = re.findall(r"\r(\{.*?\})\n", text, flags=re.DOTALL)
    parsed = []
    for ev in events:
        try:
            parsed.append(json.loads(ev))
        except Exception:
            parsed.append({"raw": ev[:200]})
    return raw, parsed, text

# ----------------------------------------------------------------
# 1) Sanity: confirm SSCMA is alive RIGHT NOW (it might have rebooted
# since the previous probe).
# ----------------------------------------------------------------
say("=== Phase 1: confirm SSCMA alive ===")
raw, parsed, _ = send_at("AT+ID?\r\n", timeout=2.0)
if not parsed:
    say("[FATAL] no SSCMA response - bailing out")
    say("        try power-cycling the device and re-running")
    sys.exit(1)
say(f"  chip ID: {parsed[0].get('data')}")
raw, parsed, _ = send_at("AT+NAME?\r\n", timeout=2.0)
say(f"  chip name: {parsed[0].get('data') if parsed else '?'}")
raw, parsed, _ = send_at("AT+VER?\r\n", timeout=2.0)
say(f"  chip ver:  {parsed[0].get('data') if parsed else '?'}")

# ----------------------------------------------------------------
# 2) Pull the FULL help text and dump it to disk so we know every
# command this firmware supports.
# ----------------------------------------------------------------
say("\n=== Phase 2: pulling full AT+HELP? ===")
raw, _, text = send_at("AT+HELP?\r\n", timeout=4.0, label="AT+HELP?")
help_path = OUT_DIR / "at_help.txt"
help_path.write_text(text, encoding="utf-8", errors="replace")
say(f"  wrote {len(text)} chars to {help_path}")
# print a digest
for line in text.splitlines():
    line = line.strip()
    if line.startswith("AT+"):
        say(f"    {line}")

# ----------------------------------------------------------------
# 3) Probe for image / sample / invoke commands.
# We'll try several SSCMA-known shapes and dump every response.
# ----------------------------------------------------------------
say("\n=== Phase 3: trying capture commands ===")

CAPTURE_CANDIDATES = [
    "AT+SAMPLE=1\r\n",          # SSCMA 1.x: kicks one frame
    "AT+SAMPLE?\r\n",
    "AT+SAMPLE=0\r\n",          # stop streaming, then start
    "AT+INVOKE=1,0,0\r\n",      # one inference
    "AT+INVOKE=1,0,1\r\n",      # one inference, draw boxes
    "AT+INVOKE?\r\n",
    "AT+ALGO?\r\n",
    "AT+ALGOS?\r\n",
    "AT+MODEL?\r\n",
    "AT+MODELS?\r\n",
    "AT+INFO?\r\n",
    "AT+ACTION?\r\n",
    "AT+CAPTURE\r\n",
    "AT+SNAP\r\n",
]

best_jpeg = None
best_label = None

def find_jpeg_in_events(events):
    """Look for a base64 'data' field in any event — common SSCMA pattern."""
    for ev in events:
        if not isinstance(ev, dict): continue
        for key in ("image", "data"):
            v = ev.get(key)
            if isinstance(v, str) and len(v) > 256:
                # try base64 decode
                try:
                    b = base64.b64decode(v + "==", validate=False)
                    if len(b) > 8 and b[0] == 0xFF and b[1] == 0xD8 and b[2] == 0xFF:
                        return b
                except Exception:
                    pass
        # nested data
        d = ev.get("data")
        if isinstance(d, dict):
            for k, v in d.items():
                if isinstance(v, str) and len(v) > 256:
                    try:
                        b = base64.b64decode(v + "==", validate=False)
                        if len(b) > 8 and b[0] == 0xFF and b[1] == 0xD8 and b[2] == 0xFF:
                            return b
                    except Exception:
                        pass
    return None

def find_raw_jpeg(text):
    """Some firmwares emit raw JPEG bytes directly, not framed JSON."""
    b = text.encode("latin-1", errors="replace")
    i = b.find(b"\xFF\xD8\xFF")
    if i < 0: return None
    j = b.find(b"\xFF\xD9", i)
    if j < 0: return None
    return b[i:j + 2]

for cmd in CAPTURE_CANDIDATES:
    try:
        raw, parsed, text = send_at(cmd, timeout=6.0)
    except Exception as e:
        say(f"  send FAIL: {e}")
        continue
    if not parsed:
        say(f"  no JSON response for {cmd.strip()}")
        continue
    say(f"  events: {len(parsed)}")
    for ev in parsed[:3]:
        try:
            preview = json.dumps(ev)[:200]
        except Exception:
            preview = str(ev)[:200]
        say(f"    -> {preview}")
    jpeg = find_jpeg_in_events(parsed) or find_raw_jpeg(text)
    if jpeg:
        say(f"  *** JPEG FOUND from {cmd.strip()}: {len(jpeg)} bytes ***")
        best_jpeg = jpeg
        best_label = cmd.strip().replace("?", "_q").replace("=", "_eq_") \
            .replace(",", "_") .replace("+", "p") .replace(" ", "_")
        # save it
        out = OUT_DIR / f"{best_label}.jpg"
        out.write_bytes(jpeg)
        say(f"  saved -> {out}")

# ----------------------------------------------------------------
# 4) If we still have nothing, force-load a built-in algorithm and
# trigger one inference; many SSCMA builds only emit images when an
# algo is loaded.
# ----------------------------------------------------------------
if best_jpeg is None:
    say("\n=== Phase 4: forcing a default algorithm ===")
    # Common patterns
    for cmd in [
        "AT+ALGO=1\r\n",
        "AT+TMODEL?\r\n",
        "AT+TMODEL=1\r\n",
        "AT+INVOKE=1,1,1\r\n",
        "AT+ACTION=\"\"\r\n",
    ]:
        try:
            raw, parsed, text = send_at(cmd, timeout=4.0)
            jpeg = find_jpeg_in_events(parsed) or find_raw_jpeg(text)
            if jpeg:
                say(f"  *** JPEG FOUND from {cmd.strip()}: {len(jpeg)} bytes ***")
                out = OUT_DIR / "phase4_capture.jpg"
                out.write_bytes(jpeg)
                say(f"  saved -> {out}")
                best_jpeg = jpeg
                break
        except Exception as e:
            say(f"  send FAIL: {e}")

say("\n=== done ===")
if best_jpeg:
    say(f"  CAPTURED a real JPEG ({len(best_jpeg)} bytes) via UART")
    say(f"  open the .jpg files in {OUT_DIR} to see what the camera sees")
else:
    say("  did not capture a JPEG yet")
    say(f"  see {help_path} for the exact command list this firmware supports")

ser.close()
f.close()
