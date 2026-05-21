#!/usr/bin/env python3
"""
himax_uart_probe.py - Talk to the Himax HX6538 AI chip directly over its
USB-CDC serial port (COM4 on this device).

What we're testing:
  Hypothesis A (SSCMA app firmware running on Himax):
    AT+ID?\r\n returns a JSON envelope like:
        \r{"type":0,"name":"AT+ID?","code":0,"data":"<chip-id>"}\n
    AT+VER? returns version string.
    AT+SAMPLE=1 returns a JPEG bytestream wrapped in JSON (data field
    contains base64-encoded JPEG).

  Hypothesis B (Himax in factory bootloader / silent):
    No response at any baud rate; chip emits at most a short power-on
    preamble.

  Hypothesis C (Himax running something else):
    Some response, but not SSCMA-shaped.

Strategy:
  1. Try the most common baud rates that SSCMA / Himax bootloader use.
  2. Watch for spontaneous output for 1.5 s (factory firmware often
     prints a banner).
  3. Send AT commands one at a time, dump every response in hex AND ASCII.
  4. Try AT+SAMPLE=1 specifically and time how long it takes.
  5. Try the WE2 bootloader 'sync' bytes too (0x55 0x55 0x55) to detect
     if the Himax is listening for firmware download.

Run:  python tools/himax_uart_probe.py [COM4]
"""
import sys, time, threading
from pathlib import Path
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM4"
LOG  = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(".pio/himax_uart_probe.log")
LOG.parent.mkdir(parents=True, exist_ok=True)
f = open(LOG, "w", encoding="utf-8", errors="replace")

def say(s):
    print(s)
    try:
        f.write(s + "\n"); f.flush()
    except UnicodeEncodeError:
        f.write(s.encode("ascii", errors="replace").decode("ascii") + "\n"); f.flush()

def hexdump(buf, max_bytes=128):
    """Return a 'XX XX XX  ABC' style hex+ascii dump."""
    if not buf:
        return "(empty)"
    show = buf[:max_bytes]
    hex_part = " ".join(f"{b:02X}" for b in show)
    ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in show)
    suffix = f"  ... (+{len(buf) - max_bytes} more)" if len(buf) > max_bytes else ""
    return f"[{len(buf)}b] {hex_part}  '{ascii_part}'{suffix}"

def looks_like_sscma_json(buf):
    """SSCMA emits framed JSON: \r{ ... }\n  Even one such frame proves it."""
    if not buf:
        return False
    s = buf.decode("latin-1", errors="replace")
    return ("\r{" in s and "}\n" in s) or s.startswith("{") and "}" in s

# ----------------------------------------------------------------
# Probe at a single baud rate.
# ----------------------------------------------------------------
def probe(baud, hold_dtr=False, hold_rts=False):
    say(f"\n========== probing {PORT} @ {baud} (DTR={hold_dtr} RTS={hold_rts}) ==========")
    try:
        ser = serial.Serial()
        ser.port     = PORT
        ser.baudrate = baud
        ser.timeout  = 0.1
        # Many USB-CDC implementations send a 'ready' edge on DTR — control it
        # explicitly so we can be deterministic.
        ser.dtr = hold_dtr
        ser.rts = hold_rts
        ser.open()
    except serial.SerialException as e:
        say(f"  open FAIL: {e}")
        return False, b""

    # 1) Listen for 1.5 s of spontaneous output (banner / boot log).
    say("  [1] listening for spontaneous output for 1.5 s ...")
    t_end = time.time() + 1.5
    spontaneous = bytearray()
    while time.time() < t_end:
        chunk = ser.read(4096)
        if chunk:
            spontaneous.extend(chunk)
        else:
            time.sleep(0.05)
    say("       " + hexdump(bytes(spontaneous), max_bytes=256))

    # 2) Send AT+ID?\r\n
    def at(cmd, settle=0.5, total_wait=2.0, label=None):
        label = label or cmd.strip()
        say(f"  [tx] {label!r}")
        ser.reset_input_buffer()
        ser.write(cmd.encode("ascii"))
        try:
            ser.flush()
        except Exception:
            pass
        time.sleep(settle)
        rx = bytearray()
        t_stop = time.time() + (total_wait - settle)
        while time.time() < t_stop:
            chunk = ser.read(4096)
            if chunk:
                rx.extend(chunk)
                t_stop = max(t_stop, time.time() + 0.3)  # extend on activity
            else:
                time.sleep(0.05)
        say("       " + hexdump(bytes(rx), max_bytes=256))
        return bytes(rx)

    r_id    = at("AT+ID?\r\n",       label="AT+ID?")
    r_name  = at("AT+NAME?\r\n",     label="AT+NAME?")
    r_ver   = at("AT+VER?\r\n",      label="AT+VER?")
    r_break = at("AT+BREAK\r\n",     label="AT+BREAK")
    r_help  = at("AT+HELP?\r\n",     label="AT+HELP?")

    # 3) WE2 bootloader sync probe (only on the lowest-traffic baud).
    if baud in (115200, 921600, 460800):
        say("  [tx] WE2 bootloader sync bytes (55 55 55)")
        ser.reset_input_buffer()
        ser.write(b"\x55\x55\x55")
        time.sleep(0.3)
        rx_sync = ser.read(4096)
        say("       " + hexdump(rx_sync, max_bytes=64))

    ser.close()

    any_response = (
        len(spontaneous) > 0 or
        any(len(b) > 0 for b in (r_id, r_name, r_ver, r_break, r_help))
    )
    sscma_like = (
        looks_like_sscma_json(r_id) or
        looks_like_sscma_json(r_name) or
        looks_like_sscma_json(r_ver) or
        looks_like_sscma_json(spontaneous)
    )

    if sscma_like:
        say(f"  ===> SSCMA JSON DETECTED at {baud}!")
    elif any_response:
        say(f"  ===> chip emitted bytes at {baud} but no SSCMA framing yet")
    else:
        say(f"  ===> total silence at {baud}")
    return sscma_like, spontaneous + r_id + r_name + r_ver + r_break + r_help

say(f"=== Himax UART probe on {PORT} ===")

results = {}
# SSCMA default is 921600 over UART per Seeed source.
# Himax WE2 bootloader uses 115200.
for cfg in [
    (921600, False, False),
    (115200, False, False),
    (460800, False, False),
    (115200, True,  True),    # try with DTR/RTS asserted in case the chip
                              # gates UART on host-ready signals
    (921600, True,  True),
]:
    baud, dtr, rts = cfg
    ok, buf = probe(baud, hold_dtr=dtr, hold_rts=rts)
    results[cfg] = (ok, len(buf))

say("\n=== summary ===")
for cfg, (ok, n) in results.items():
    say(f"  {cfg}: SSCMA={ok}  bytes_received={n}")

print(f"\n[done] log -> {LOG}")
f.close()
