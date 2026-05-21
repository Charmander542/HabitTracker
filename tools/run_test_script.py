"""
run_test_script.py — Send a scripted sequence of commands to the
HabitTracker serial console and capture all output to a log file.

Usage:
  python tools/run_test_script.py <port> <outfile> [--noreset] [--boot-delay <sec>]

The script block is hard-coded below. Lines are either:
  ("cmd", "<verb + args>", wait_seconds)      send a line, then read for N s
  ("wait", "<reason>", seconds)               just read for N s
  ("banner", "<text>", 0)                     write a visible marker to the log

The device is reset once at the start (same DTR/RTS dance as
capture_serial.py) unless --noreset is supplied, then we give it
BOOT_DELAY seconds to finish setup() before sending the first command.
"""
import sys
import time
import threading
import serial


SCRIPT = [
    ("banner", "======== BOOT CAPTURE ========", 0),
    ("wait",   "let setup() complete", 4.0),

    ("banner", "======== STATS ========", 0),
    ("cmd",    "stats", 1.0),

    ("banner", "======== DISPLAY FILLS ========", 0),
    ("cmd",    "disp red",    1.2),
    ("cmd",    "disp green",  1.2),
    ("cmd",    "disp blue",   1.2),
    ("cmd",    "disp white",  1.2),
    ("cmd",    "disp black",  1.0),
    ("cmd",    "disp bars",   1.2),

    ("banner", "======== DUCK FRAMES ========", 0),
    ("cmd",    "duck 0", 1.0),
    ("cmd",    "duck 1", 1.0),
    ("cmd",    "duck 2", 1.0),
    ("cmd",    "duck 3", 1.0),
    ("cmd",    "duck 4", 1.0),
    ("cmd",    "duck 5", 1.0),
    ("cmd",    "duck 6", 1.0),

    ("banner", "======== LED COLOURS ========", 0),
    ("cmd",    "led 255,0,0",   1.0),
    ("cmd",    "led 0,255,0",   1.0),
    ("cmd",    "led 0,0,255",   1.0),
    ("cmd",    "led 255,255,255", 1.0),
    ("cmd",    "led off",       0.5),

    ("banner", "======== HAPTIC PATTERNS ========", 0),
    ("cmd",    "haptic buzz",        1.0),
    ("cmd",    "haptic countdown",   1.0),
    ("cmd",    "haptic heartbeat",   1.0),
    ("cmd",    "haptic celebration", 1.5),
    ("cmd",    "haptic sos",         1.5),
    ("cmd",    "haptic none",        0.5),

    ("banner", "======== STORAGE ========", 0),
    ("cmd",    "ls /",             1.0),
    ("cmd",    "cat /pet_config.json", 1.0),
    ("cmd",    "cat /habits.json",     1.5),

    ("banner", "======== BUTTON IDLE READING (firmware blocks 10s) ========", 0),
    ("cmd",    "btn", 11.0),

    ("banner", "======== KNOB IDLE READING (firmware blocks 10s) ========", 0),
    ("cmd",    "knob", 11.0),

    ("banner", "======== RESUME APP ========", 0),
    ("cmd",    "test off",  1.0),
    ("cmd",    "state idle", 3.0),

    ("banner", "======== DONE ========", 0),
]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    port = sys.argv[1]
    outfile = sys.argv[2]
    noreset = "--noreset" in sys.argv
    boot_delay = 0.3
    if "--boot-delay" in sys.argv:
        i = sys.argv.index("--boot-delay")
        boot_delay = float(sys.argv[i + 1])

    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.1
    s.dtr = False
    s.rts = False
    s.open()

    if not noreset:
        try:
            s.dtr = False
            s.rts = True
            time.sleep(0.1)
            s.rts = False
            time.sleep(0.05)
            s.reset_input_buffer()
        except Exception as e:
            print(f"reset toggle failed: {e}", file=sys.stderr)

    log = open(outfile, "wb")
    # Live-tee reader thread
    stop_flag = threading.Event()

    def reader():
        while not stop_flag.is_set():
            try:
                data = s.read(4096)
            except Exception:
                break
            if data:
                log.write(data)
                log.flush()
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()

    t = threading.Thread(target=reader, daemon=True)
    t.start()

    if boot_delay > 0:
        time.sleep(boot_delay)

    for action, payload, wait in SCRIPT:
        if action == "banner":
            msg = f"\n[HOST] {payload}\n".encode("utf-8")
            log.write(msg)
            sys.stdout.buffer.write(msg)
            sys.stdout.buffer.flush()
        elif action == "cmd":
            line = (payload + "\n").encode("utf-8")
            s.write(line)
            s.flush()
            banner = f"[HOST] >>> {payload}\n".encode("utf-8")
            log.write(banner)
            sys.stdout.buffer.write(banner)
            sys.stdout.buffer.flush()
            time.sleep(wait)
        elif action == "wait":
            msg = f"[HOST] waiting {wait}s ({payload})\n".encode("utf-8")
            log.write(msg)
            sys.stdout.buffer.write(msg)
            sys.stdout.buffer.flush()
            time.sleep(wait)

    stop_flag.set()
    time.sleep(0.2)
    log.close()
    s.close()
    print(f"\n[done — log in {outfile}]")


if __name__ == "__main__":
    main()
