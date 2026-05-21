"""
Capture serial output from a COM port for N seconds and write it to a file.
Usage: python capture_serial.py <port> <baud> <seconds> <outfile>

The device is reset once via DTR/RTS toggle at start so we catch the boot banner.
"""
import sys, time, serial

port, baud, secs, outfile = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]

noreset = "--noreset" in sys.argv
s = serial.Serial()
s.port = port
s.baudrate = baud
s.timeout = 0.1
# Set DTR/RTS state BEFORE opening so the port-open doesn't glitch the
# ESP32's GPIO0/EN lines into download mode. On the Watcher's CH342:
#   DTR controls GPIO0 (BOOT)  — want HIGH to boot normally  (dtr=False -> line HIGH)
#   RTS controls EN   (RESET)  — want HIGH to release reset (rts=False -> line HIGH)
s.dtr = False
s.rts = False
s.open()
if not noreset:
    try:
        # proper normal-boot reset: pulse EN low while leaving BOOT high
        s.dtr = False    # GPIO0 stays HIGH (normal boot)
        s.rts = True     # EN LOW  (reset asserted)
        time.sleep(0.1)
        s.rts = False    # EN HIGH (reset released -> normal boot)
        time.sleep(0.05)
        s.reset_input_buffer()
    except Exception as e:
        print(f"reset toggle failed: {e}", file=sys.stderr)

end = time.time() + secs
with open(outfile, "wb") as f:
    while time.time() < end:
        data = s.read(4096)
        if data:
            f.write(data)
            f.flush()
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
s.close()
print(f"\n[capture complete, {secs}s]")
