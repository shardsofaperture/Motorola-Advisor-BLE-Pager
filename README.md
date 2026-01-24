# PagerBridge (ESP32-S3 → Motorola Advisor II DATA IN)

## What this does (no RF)
This project turns a Seeed XIAO ESP32-S3 into a BLE/Serial bridge that injects a **POCSAG baseband NRZ stream** directly into a Motorola Advisor II logic board. The RF board is removed; the ESP32 drives the **DATA injection node** on the logic board (continuity to the RF board’s **TA31142 pin 15** net). The pager logic board decodes POCSAG exactly like it would from RF.  

## Wiring / Injection (Advisor II)
### Required
- **ESP32 GND → pager GND** (common ground is mandatory).  
  - Tie to **TA31142 pin 19** or a nearby ground plane on the RF board.
- **ESP32 GPIO3 (D2) → TA31142 pin 15 net**  
  - **Recommended:** insert a **1k–2k series resistor** in-line from GPIO3 to the injection node.
  - **Best practice:** isolate the TA31142 output by **lifting pin 15** or **cutting the trace**, then inject on the **downstream side** toward the logic board.

### Drive style
By default the firmware drives **push-pull** for TA31142 injection:
- **Logic 1 / idle**: GPIO3 driven **HIGH**
- **Logic 0**: GPIO3 driven **LOW**

You can also choose **open-drain** if you need to share a line:
- **Logic 1 / idle**: GPIO3 set to **INPUT (hi-Z)**
- **Logic 0**: GPIO3 driven **OUTPUT LOW**

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
Shows a single-line summary (capcodes/baud/invert/pins/pages).
```
STATUS
```

### SET CAPCODE <int>
Sets the individual capcode (and group to IND+1 unless already set explicitly).
```
SET CAPCODE 0123456
```

### SET CAPIND <int>
```
SET CAPIND 0123456
```

### SET CAPGRP <int>
```
SET CAPGRP 0123457
```

### SET CAPS <ind> <grp>
```
SET CAPS 0123456 0123457
```

### SET BAUD <512|1200|2400>
```
SET BAUD 1200
```

### SET INVERT <0|1>
```
SET INVERT 1
```

### SET OUTPUT <PUSH_PULL|OPEN_DRAIN>
```
SET OUTPUT PUSH_PULL
```

### SET AUTOPROBE <0|1>
Enable/disable a one-time probe on boot (tries the saved IND then GRP capcodes once).
```
SET AUTOPROBE 1
```

### PAGE <text>
Send a page to the configured individual capcode (CAPIND).
```
PAGE Hello
```

### PAGEI <text>
Force a page to the individual capcode.
```
PAGEI Hello
```

### PAGEG <text>
Force a page to the group capcode.
```
PAGEG Hello
```

### PAGE <capcode> <text>
Send to a specific capcode.
```
PAGE 0123457 Hello
```

### TEST CARRIER <ms>
Transmit a repeating 0xAA pattern for wiring verification (uses current BAUD/output/invert).
```
TEST CARRIER 3000
```

### SET_RATE <512|1200|2400>
```
SET_RATE 512
```

### SET_INVERT <0|1>
```
SET_INVERT 0
```

### SET_MODE <opendrain|pushpull>
```
SET_MODE opendrain
```

### SEND_TEST
Send a 2-second 1010 test pattern, then a full preamble + sync + one batch.
```
SEND_TEST
```

### SEND_ADDR <capcode> <function 0-3>
```
SEND_ADDR 123456 0
```

### SEND_MSG <capcode> <function 0-3> <ascii>
```
SEND_MSG 123456 0 "HELLO"
```

### SEND_CODEWORDS <hex...>
Inject already-encoded 32-bit codewords (useful for bring-up).
```
SEND_CODEWORDS 0x7CD215D8 0x12345678 0x7A89C197
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
Force-save settings to NVS (capcodes/baud/invert/autoprobe + recent pages).
```
SAVE
```

## Quick-start
1. Wire GND and GPIO3 to the pager DATA injection node (with series resistor).
2. Flash the firmware (PlatformIO).
3. Open Serial Monitor at 115200 and confirm the boot banner prints settings.
4. Send a page:
   ```
   PAGE Hello
   ```
5. If the pager wakes but doesn’t decode, toggle invert:
   ```
   SET INVERT 0
   ```
6. If you need auto-probe, wire ALERT_GPIO and use `PROBE START` or `PROBE BINARY`.

## Recommended settings (Advisor II TA31142 injection)
- **BAUD:** 512  
- **INVERT:** 0  
- **OUTPUT:** PUSH_PULL  
- **CAPCODES:** 123456 (individual) / 123457 (group)

## Injecting into logic board (no RF board)
When the RF/IF board is removed, the logic board still expects **sliced data** on the node that
normally connects to the RF detector’s **FSK OUT / sliced data** line. Wire:
- **GND → logic board GND**
- **DATA → logic board DATA-IN node** (the same net that previously went to RF board FSK OUT)

## Troubleshooting (top 3)
1. **Wrong pad / net**: confirm you are on the logic board’s DATA-IN node (the RF board’s FSK OUT net).
2. **Wrong polarity**: try toggling invert (`SET_INVERT 1`) if you see activity but no decode.
3. **Missing pull-up / mode mismatch**: switch between push-pull and open-drain (`SET_MODE opendrain`).

## Quick Test (wiring verification)
1. Check current settings:
   ```
   STATUS
   ```
2. Send a carrier:
   ```
   TEST CARRIER 3000
   ```
3. Page individual capcode:
   ```
   PAGEI test
   ```
4. Page group capcode:
   ```
   PAGEG test
   ```

## Expected behavior / troubleshooting
- **No serial output**  
  Ensure `ARDUINO_USB_CDC_ON_BOOT=1` is enabled (already in `platformio.ini`) and you’re using 115200 baud.
- **Pager wakes but won’t decode**  
  Toggle `SET INVERT 0/1`. The DATA line polarity must match the logic board.
- **Nothing happens at all**  
  Verify **common ground** and the DATA injection node continuity to the TA31142 pin 15 net.
- **Probe doesn’t auto-save capcodes**  
  `PROBE START` and `PROBE BINARY` **require ALERT_GPIO**. If ALERT_GPIO isn’t wired, you’ll get `ERROR PROBE NO_ALERT_GPIO`. Use `PROBE ONESHOT` and watch the pager manually.

### Notes
- Leading zeros are just formatting (e.g., **0123456 == 123456**).
