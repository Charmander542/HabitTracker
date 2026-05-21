"""
SSCMA Himax camera full diagnostic sweep.
Runs: boot capture, camstatus, cambus, camsweep, camblind, camimg.
Usage: python sscma_diag.py [COM3]
"""
import sys, time, serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
BAUD = 115200
OUT  = "sscma_diag.log"

s = serial.Serial()
s.port = PORT
s.baudrate = BAUD
s.timeout = 0.1
s.dtr = False
s.rts = False
s.open()

# Reset into normal boot
s.rts = True
time.sleep(0.1)
s.rts = False
time.sleep(0.05)
s.reset_input_buffer()

log = open(OUT, "wb")

def pump(seconds, label=""):
    if label:
        banner = f"\n===== {label} (for {seconds}s) =====\n"
        log.write(banner.encode()); log.flush()
        sys.stdout.write(banner); sys.stdout.flush()
    end = time.time() + seconds
    while time.time() < end:
        data = s.read(4096)
        if data:
            log.write(data); log.flush()
            try:
                sys.stdout.write(data.decode("utf-8", "replace"))
            except Exception:
                sys.stdout.write(repr(data))
            sys.stdout.flush()

def send(cmd, wait_s):
    s.write((cmd + "\r\n").encode())
    s.flush()
    pump(wait_s, f"CMD: {cmd}")

pump(12.0, "BOOT + setup()")

# Diagnostic sequence.
send("caminit",   8.0)
send("camstatus", 2.0)
send("cambus",    6.0)
send("camsweep",  14.0)
send("camblind",  4.0)
send("camblind AT+NAME?", 4.0)
send("campoke",   4.0)
send("camimg",    8.0)

log.close()
s.close()
print(f"\n[saved to {OUT}]")
