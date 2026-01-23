# PagerBridge (ESP32-S3 → Motorola Advisor II DATA IN)

## What this does (no RF)
This project turns a Seeed XIAO ESP32-S3 into a BLE/Serial bridge that injects a **POCSAG baseband NRZ stream** directly into a Motorola Advisor II logic board. The RF board is removed; the ESP32 drives the **DATA injection node** on the logic board (continuity to the RF board’s **TA31142 pin 15** net). The pager logic board decodes POCSAG exactly like it would from RF.  

## Wiring
### Required
- **ESP32 GND → pager GND** (common ground is mandatory).
- **ESP32 GPIO2 → pager DATA injection test point**  
  - This test point has continuity to the RF board TA31142 pin 15 net via the board-to-board connector.
  - **Recommended:** put a **1k–4.7k series resistor** in-line from GPIO2 to the DATA node.

### Drive style
The firmware emulates **open-drain** drive on GPIO2:
- **Active / “0”**: GPIO2 driven **OUTPUT LOW**
- **Idle / “1”**: GPIO2 set to **INPUT (hi-Z)**

### Optional (for automatic probe/hit detection)
If you want PROBE auto-detect to work, wire the pager’s alert indication to **ALERT_GPIO** (see `kAlertGpio` in `src/main.cpp`):
- Suggested sources: **buzzer drive** line or **LED/backlight** line.
- Use a simple conditioning network if needed (e.g., resistor divider or transistor) to make a clean 3.3 V logic signal.

## Build & flash (PlatformIO)
1. Install **PlatformIO** in VS Code.
2. Open this repo in VS Code.
3. Build and flash:
   ```bash
   platformio run -t upload
   ```
4. Open serial monitor at **115200**:
   ```bash
   platformio device monitor
   ```
   (`monitor_speed = 115200` is already configured in `platformio.ini`.)

## How to use
### BLE interface
- **Device name:** `PagerBridge`
- **Service UUID:** `1b0ee9b4-e833-5a9e-354c-7e2d486b2b7f`
- **RX characteristic (write):** `1b0ee9b4-e833-5a9e-354c-7e2d496b2b7f`
- **Status characteristic (read/notify):** `1b0ee9b4-e833-5a9e-354c-7e2d4a6b2b7f`

You can use any BLE client (nRF Connect, LightBlue, etc.) to write commands or page text to the **RX characteristic**.

### Serial console
Open the serial monitor at 115200 baud. Type the same commands as BLE (ending with newline) to control the pager.

## Commands (BLE or Serial)
Commands are **plain text** and case-insensitive. Examples show the exact text to send.

### STATUS
Shows a single-line summary (capcode/baud/invert/pins/pages).
```
STATUS
```

### SET CAPCODE <int>
```
SET CAPCODE 91833
```

### SET BAUD <512|1200|2400|6400>
```
SET BAUD 1200
```

### SET INVERT <0|1>
```
SET INVERT 1
```

### SET AUTOPROBE <0|1>
Enable/disable a one-time probe on boot (tries the saved capcode once).
```
SET AUTOPROBE 1
```

### PAGE <text>
Send a page to the configured capcode.
```
PAGE Hello from PagerBridge
```

### PAGE <capcode> <text>
Send to a specific capcode.
```
PAGE 91833 Test page
```

### LIST
List stored pages (newest first). Output is chunked into status notifications and also printed to Serial.
```
LIST
```

### RESEND <index>
Resend a stored page by index from LIST (0 = newest).
```
RESEND 0
```

### CLEAR
Clear stored pages.
```
CLEAR
```

### PROBE START <start> <end> <step>
Sequentially probe capcodes and auto-detect a “hit” using ALERT_GPIO.
```
PROBE START 90000 92000 1
```

### PROBE BINARY <start> <end>
Binary-ordered probing (faster coverage) with ALERT_GPIO hit detection.
```
PROBE BINARY 90000 92000
```

### PROBE STOP
Stop any active probe.
```
PROBE STOP
```

### PROBE ONESHOT <cap1> <cap2> ...
Send probe pages once per capcode **without** auto-detection (manual watch).
```
PROBE ONESHOT 91833 91834 91835
```

### SAVE
Force-save settings to NVS (capcode/baud/invert/autoprobe + recent pages).
```
SAVE
```

## Quick-start
1. Wire GND and GPIO2 to the pager DATA injection node (with series resistor).
2. Flash the firmware (PlatformIO).
3. Open Serial Monitor at 115200 and confirm the boot banner prints settings.
4. Send a page:
   ```
   PAGE 91833 Hello
   ```
5. If the pager wakes but doesn’t decode, toggle invert:
   ```
   SET INVERT 0
   ```
6. If you need auto-probe, wire ALERT_GPIO and use `PROBE START` or `PROBE BINARY`.

## Expected behavior / troubleshooting
- **No serial output**  
  Ensure `ARDUINO_USB_CDC_ON_BOOT=1` is enabled (already in `platformio.ini`) and you’re using 115200 baud.
- **Pager wakes but won’t decode**  
  Toggle `SET INVERT 0/1`. The DATA line polarity must match the logic board.
- **Nothing happens at all**  
  Verify **common ground** and the DATA injection node continuity to the TA31142 pin 15 net.
- **Probe doesn’t auto-save capcode**  
  `PROBE START` and `PROBE BINARY` **require ALERT_GPIO**. If ALERT_GPIO isn’t wired, you’ll get `ERROR PROBE NO_ALERT_GPIO`. Use `PROBE ONESHOT` and watch the pager manually.
