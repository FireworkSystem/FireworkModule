# Fireworks Module — CLAUDE.md

## Project overview

This is the firmware for the **ESP32 WROOM** which acts as a **slave module**
on a fireworks launching line. Multiple modules (up to 31) connect to one
Heltec head via RS485 half-duplex bus.

Each module controls **8 relay channels** for firing igniters, displays a
touch UI on an ILI9341 screen, and syncs firing plan files from the head.

---

## Hardware

| Component | Details |
|---|---|
| Board | ESP32 WROOM-32 (240MHz, 4MB Flash) |
| Display | ILI9341 TFT with XPT2046 touch (same rotation as keypad: portrait) |
| RS485 | MAX485 module |
| Relays | HW-316 relay module (up to 8 channels) |
| Storage | LittleFS on internal flash |

### Pin assignments

| Signal | GPIO | Notes |
|---|---|---|
| RS485 TX (DI) | 17 | Serial2 |
| RS485 RX (RO) | 16 | Serial2 |
| RS485 DE/RE | 26 | Direction control |
| TFT — configured via TFT_eSPI User_Setup.h | | |
| Relay control 1-4 | 18, 19, 21, 22 | Digital output |
| Relay control 5-8 | 23, 25, 32, 33 | Digital output |
| Igniter detect 1-4 | 32, 33, 25, 23 | INPUT, HIGH=connected (placeholder) |

### Strapping pins to be careful with on WROOM

- GPIO 0 → boot (must be HIGH for normal boot)
- GPIO 2 → must be LOW at boot
- GPIO 12 → must be LOW at boot (3.3V flash)
- GPIO 15 → boot log

---

## File structure

```
src/
├── main.cpp             # Entry point — setup() and loop()
├── RS485Protocol.h      # Frame format, CRC-16, base64, constants (shared with head)
├── RS485Node.h/.cpp     # Base class — UART + frame TX/RX
├── RS485Slave.h/.cpp    # Slave — discovery, reconnection, command handling
├── FileSyncSlave.h/.cpp # File sync — compares /plans/ with head, downloads missing
├── RelayView.h/.cpp     # Main UI — 2×4 grid of 8 relay buttons (portrait)
├── RelayButton.h/.cpp   # Single relay button — state machine + drawing
├── Keypad.h/.cpp        # Numeric keypad — assigns shot numbers to relays
├── SplashScreen.h/.cpp  # AnimatedGIF splash screen on boot
├── TopSequencer.h/.cpp  # Timer-based firing sequencer
├── State.h              # State enum: NotConnected=-1, Preparation=0..Fin=5
├── top.h                # Top struct: id, timeInTenths, alert, separatorBefore
└── logo_gif.h           # Generated from GIF file: python3 convert.py
```

---

## Architecture

```
Heltec Head (RS485 Master)
    ↓ RS485 half-duplex (115200 baud, MAX485)
RS485Slave (this board)
    ├── FileSyncSlave    → sync /plans/ files from head
    ├── RelayView        → touch UI for relay assignment
    └── TopSequencer     → fires relays at correct timeInTenths
```

---

## RS485 Protocol (same as head)

### Frame format
```
[STX 0x02][DST][SRC][LEN_H][LEN_L][JSON payload…][CRC_H][CRC_L][ETX 0x03]
```

### Module addressing
- Modules use address `0xFE` before registration
- After welcome, they use their assigned address `0x01–0x1F`
- Heartbeat timeout: 15s without ping → re-register

### Discovery flow
```
Boot → sendHello() every 3s until welcome received
Welcome received → onRegistered() fires → FileSyncSlave::requestSync()
Sync complete → onSyncComplete() fires
```

### Module reconnection
If no frame received for `connectionTimeoutMs` (15s), module resets
`m_registered=false`, `m_addr=0`, and re-sends hello.

---

## Display UI

### Main view (portrait, rotation 0)
2×4 grid of 8 relay buttons, each 120×80 px:

```
┌───────────┬───────────┐
│  Relay 1  │  Relay 2  │
├───────────┼───────────┤
│  Relay 3  │  Relay 4  │
├───────────┼───────────┤
│  Relay 5  │  Relay 6  │
├───────────┼───────────┤
│  Relay 7  │  Relay 8  │
└───────────┴───────────┘
```

### Relay button states
- **Red** `DISCONNECTED` — igniter absent, tap ignored
- **Orange** `CONNECTED` — igniter detected, no shot assigned
- **Green** `ASSIGNED` — shot number confirmed, shows shot number

### Keypad
Full-screen portrait numeric entry. Opens on tap of orange/green button.
- Empty buffer → OK shows "Cancel"
- Digits entered → OK shows "OK"
- Cancel returns to previous state without change
- Pre-fills with existing shot number when re-opening assigned button

### Touch calibration
- Stored in `/TouchCalV` on LittleFS
- Calibrated once on first boot (or if file lost)
- Uses `Keypad::calibrateOnly()` — calibrates without drawing keypad UI

---

## File sync

- Plans stored in `/plans/` on LittleFS
- Comparison by `hash` field (SHA-256) in JSON — not recomputed
- Sync triggered automatically after registration
- Files not on head are deleted from module
- Missing/outdated files are downloaded from head

### File format (plan JSON)
```json
{
  "name": "Show 1",
  "hash": "de395fd691447d87655d973695fe62282bfdf4989921c4ba438b95e1eb82c2f7",
  "audioFilePath": "/storage/.../show1.mp3",
  "audioTotalDuration": 180,
  "tops": [
    {"id": 1, "timeInTenths": 50, "alert": false, "separatorBefore": false}
  ]
}
```

---

## TopSequencer

Fires relay callbacks at correct `timeInTenths` timestamps.

```cpp
sequencer.onFire([](int id) { /* trigger relay for this id */ });
sequencer.load(tops, count);  // sorts by timeInTenths automatically
sequencer.start();            // must call update() every loop()
sequencer.pause();
sequencer.resume();
sequencer.stop();
sequencer.jumpTo(150);        // jump to t=15.0s
```

Time unit: **tenths of a second** (100ms per unit).

---

## Splash screen

GIF played once on boot from PROGMEM.

### Generating logo_gif.h from a GIF file
```python
# convert.py — run in project root with logo.gif present
with open('logo.gif', 'rb') as f:
    data = f.read()
with open('src/logo_gif.h', 'w', encoding='utf-8') as out:
    out.write('#pragma once\n#include <pgmspace.h>\n')
    out.write('const uint8_t logo_gif[] PROGMEM = {\n')
    for i in range(0, len(data), 16):
        out.write('  ' + ', '.join(f'0x{b:02X}' for b in data[i:i+16]) + ',\n')
    out.write('};\n')
    out.write(f'const uint32_t logo_gif_size = {len(data)};\n')
```

Add to platformio.ini: `bitbank2/AnimatedGIF@^2.1.0`

---

## Key decisions & known issues

- `RELAY_COUNT = 8` defined in `RelayView.h` — also used in `main.cpp` for DETECT_PINS
- Igniter detection pins are placeholders — replace `const bool connected = (i == 0)`
  with `digitalRead(DETECT_PINS[i]) == HIGH` when hardware is ready
- `FileSyncSlave` owns `onRegistered`, `onCommand`, `onFileReady` callbacks —
  use `fileSync.onRegistered()` not `slave.onRegistered()` directly
- `sync` is a reserved POSIX identifier — use `fileSync` as variable name
- `RS485Slave()` default constructor works (no `()` — most vexing parse)
- Touch calibration is shared with keypad — `RelayView::begin()` calls
  `m_keypad.calibrateOnly()` to calibrate without showing keypad UI
- Keypad ghost press fix: `press(false)` called twice in `show()` to flush
  `justPressed()` state from the tap that opened the keypad
- Color issue: ILI9341 may show BGR — fix in TFT_eSPI `User_Setup.h`
  with `#define TFT_RGB_ORDER TFT_BGR`, NOT in code

---

## platformio.ini key settings

```ini
platform = espressif32@6.6.0
board = esp32dev
framework = arduino
board_build.filesystem = littlefs
lib_deps =
    bodmer/TFT_eSPI@^2.5.43
    bblanchon/ArduinoJson@^7.0.0
    bitbank2/AnimatedGIF@^2.1.0
```