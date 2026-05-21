"""
Cold-boot SSCMA probe. Captures boot output, then runs `camcold` and
several probes that BYPASS the AVAILABLE/SYNC handshake to see what
the Himax actually emits after a fresh reset.
"""
import sys, time, serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
BAUD = 115200
OUT  = "sscma_cold.log"

s = serial.Serial()
s.port = PORT
s.baudrate = BAUD
s.timeout = 0.1
s.dtr = False
s.rts = False
s.open()
s.rts = True
time.sleep(0.1)
s.rts = False
time.sleep(0.05)
s.reset_input_buffer()

log = open(OUT, "wb")

def pump(seconds, label=""):
    if label:
        b = f"\n===== {label} ({seconds}s) =====\n"
        log.write(b.encode()); log.flush()
        sys.stdout.write(b); sys.stdout.flush()
    end = time.time() + seconds
    while time.time() < end:
        d = s.read(4096)
        if d:
            log.write(d); log.flush()
            try: sys.stdout.write(d.decode("utf-8", "replace"))
            except: sys.stdout.write(repr(d))
            sys.stdout.flush()

def send(cmd, w):
    s.write((cmd + "\r\n").encode())
    s.flush()
    pump(w, f"CMD: {cmd}")

pump(11.0, "BOOT + setup()")
send("camcold 5000",       7.0)
send("camstatus",          2.0)
send("cambus",             6.0)
send("camblind AT+ID?",    4.0)
send("camblind AT+NAME?",  4.0)
send("camblind AT+VER?",   4.0)
send("caminit",            6.0)
send("camimg",             8.0)

log.close()
s.close()
print(f"\n[saved to {OUT}]")
