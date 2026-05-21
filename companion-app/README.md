# Habit Companion Studio

Desktop companion app for the [Seeed SenseCAP Watcher](https://www.seeedstudio.com/SenseCAP-Watcher-W1-A-p-5979.html) Habit Companion firmware.  
Design your pixel pet, manage habits, and view device photos — all from a single cozy desktop app.

---

## Requirements

- Python 3.10+
- Windows / macOS / Linux

```bash
pip install -r requirements.txt
```

---

## Running

### Demo mode (no device needed)
```bash
python main.py --demo
```
Runs with mock data so you can build and preview the full UI without a physical Watcher connected.

### Normal mode (device over USB)
```bash
python main.py
```
The app auto-discovers the Watcher by USB VID (CH340 / CP210x).  
It polls `GET_STATUS` every 5 s and reconnects automatically on disconnect.

---

## Project structure

```
companion-app/
├── main.py               App entry point, window setup, navigation
├── theme.py              All colors, fonts, spacing constants
├── requirements.txt
│
├── core/
│   ├── config_model.py   CompanionConfig dataclass + JSON serialization
│   ├── serial_bridge.py  Background-thread USB serial bridge
│   └── image_utils.py    Font downloading + OS registration
│
├── components/
│   ├── nav_rail.py       Collapsible left navigation sidebar
│   ├── stat_slider.py    Custom pill-shaped personality slider
│   ├── color_picker.py   Circular swatch palette picker
│   └── pet_preview.py    Live-animated pet sprite canvas
│
└── views/
    ├── dashboard.py      Device status hub — vitality ring, habit grid, feed
    ├── builder.py        Companion creator — appearance + personality stats
    ├── habits.py         Habit manager — add/edit/reorder, modal panel
    └── gallery.py        Photo viewer — pull from device, hover overlays
```

---

## Serial protocol

Text-based, newline-terminated:

| Direction    | Message                          | Description                        |
|--------------|----------------------------------|------------------------------------|
| App → Device | `PING`                           | Heartbeat                          |
| App → Device | `GET_STATUS`                     | Request current state JSON         |
| App → Device | `GET_PHOTOS`                     | Initiate photo list transfer       |
| App → Device | `SEND_CONFIG <json_payload>`     | Push full CompanionConfig          |
| Device → App | `PONG`                           | Heartbeat reply                    |
| Device → App | `STATUS <json>`                  | Vitality, streak, habit progress   |
| Device → App | `PHOTO_START <name> <bytes>`     | Begin photo transfer               |
| Device → App | `PHOTO_DATA <base64_chunk>`      | Photo chunk                        |
| Device → App | `PHOTO_END`                      | Photo transfer complete            |
| Device → App | `CONFIG_ACK`                     | Config received and applied        |
| Device → App | `ERROR <message>`                | Error from device                  |

---

## Config backup

Config auto-saves to `~/.habit-companion/config.json` on close.  
Fonts are cached at `~/.habit-companion/fonts/` after first download.
