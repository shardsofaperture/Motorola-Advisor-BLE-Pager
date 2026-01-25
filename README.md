# PagerBridge (ESP32-S3 → Motorola Advisor II DATA IN)

> **Status: Work in progress / not functional yet.** This project is still under active development and **does not currently work with Motorola Advisor II pagers**. Do not rely on it for production use.

## What this does (no RF)
This project turns a Seeed XIAO ESP32-S3 into a BLE/Serial bridge that injects a **POCSAG baseband NRZ stream** directly into a Motorola Advisor II logic board. The ESP32 drives the RF board connector **pin labeled “DATA”** (post-limiter, post-slicer, pre-ASIC), which is the correct injection point per the service manual. The pager logic board decodes POCSAG exactly like it would from RF.  

## Wiring / Injection (Advisor II)
### Required
- **ESP32 GND → pager GND** (common ground is mandatory).  
  - Tie to a nearby RF board ground plane.
- **ESP32 GPIO3 (D2) → RF board connector pin labeled “DATA”**  
  - **Recommended:** insert a **1k–2k series resistor** in-line from GPIO3 to the DATA pin.
  - The DATA pin is **post-limiter, post-slicer, pre-ASIC** per the service manual.

### Drive style
By default the firmware drives **open-drain** for DATA injection:
- **Logic 1 / idle**: GPIO3 set to **INPUT (hi-Z)**
- **Logic 0**: GPIO3 driven **OUTPUT LOW**

You can also choose **push-pull** if you need it:
- **Logic 1 / idle**: GPIO3 driven **HIGH**
- **Logic 0**: GPIO3 driven **LOW**

### Optional (for automatic probe/hit detection)
If you want PROBE auto-detect to work, wire the pager’s alert indication to **ALERT_GPIO** (see `kAlertGpio` in `src/main.cpp`):
- Suggested sources: **buzzer drive** line or **LED/backlight** line.
- Use a simple conditioning network if needed (e.g., resistor divider or transistor) to make a clean 3.3 V logic signal.

## Correct Injection Point (Verified by Service Manual)
- **Use the RF board connector pin labeled “DATA.”**
- This node is **post-limiter, post-slicer, pre-ASIC** and carries raw POCSAG NRZ.
- **Do not inject** at TA31142 pads, discriminator/audio points, or **TSP pads**—those are not valid injection points for raw NRZ.
- The DATA pin behaves as an **open-collector line** (internal pull-up); the injector must only **pull low** or **float**.

**Bring-up steps (service-manual compliant)**
1. Wire GPIO3 to the **RF board DATA pin** with a 1k–2k series resistor.
2. Run `DEBUG_SCOPE` on the DATA pin to confirm 512 bps timing.
3. Run `AUTOTEST_FAST 60` to sweep invert/function/preamble.
4. Lock the winning combo once the pager alerts.

## Bring-up checklist (known: POCSAG, 512 bps, capcode 123456)
**Known values**
- **Protocol:** POCSAG
- **Baud:** 512 bps
- **Capcode:** 123456

**Recommended starting config**
- **OUTPUT:** `OPEN_DRAIN` (open-collector)
- Sweep **INVERT/FUNCTION/PREAMBLE** via `AUTOTEST_FAST` instead of changing capcodes.

**Suggested test flow**
1. Run `DEBUG_SCOPE` at 512 on the RF DATA pin.
2. Run `AUTOTEST_FAST 60` while monitoring for an alert.
3. If any beep/alert is observed, lock that combo and use `SEND_MIN_LOOP` for longer.

## No RF service bring-up
If AUTOTEST never decodes, confirm the wiring is on the **RF board DATA pin** and the line is **open-collector**. Other pads (TA31142, discriminator audio, TSP) are not valid for raw NRZ injection.

### Injection profiles (signal styles)
- **NRZ_SLICED**: raw NRZ bits at the baud rate. Intended for **post-slicer digital nodes** (TSP2).
- **NRZ_PUSH_PULL**: raw NRZ, but push-pull drive for CMOS-style inputs.
- **MANCHESTER**: bi-phase level (each bit is high→low or low→high at half-bit rate).
- **DISCRIM_AUDIO_FSK**: square-wave approximation of discriminator audio (1200/2200 Hz per bit). Intended for **discriminator/analog/slicer-input** nodes.
- **SLICER_EDGE_PULSE**: emit a short low pulse on each NRZ transition. Intended for **AC-coupled nodes**.

### Multi-pin brute force
Set a list of candidate GPIOs so you can move the injection wire without changing commands:
```
SET_GPIO_LIST 3,4
AUTOTEST2 123456 120
```

Notes:
- **123456 and 123457 share the same POCSAG address** because addressing is based on **capcode / 2**. The function bits differentiate them.

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
Shows a single-line summary including baud/invert/output/data GPIO plus default function/preamble.
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

### SET OUTPUT <PUSH_PULL|OPEN_DRAIN|OPEN_COLLECTOR>
```
SET OUTPUT PUSH_PULL
```

### SET_GPIO <pin>
Change the DATA GPIO (re-initializes the transmitter).
```
SET_GPIO 3
```

### SET_GPIO_LIST <pin1,pin2,...>
Store a list of candidate DATA GPIOs for AUTOTEST2 (used when moving the injection wire).
```
SET_GPIO_LIST 3,4
```

### SET_IDLE <0|1>
Set idle polarity (1 = idle high, 0 = idle low).
```
SET_IDLE 1
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

### DEBUG_SCOPE
Emit a 2-second 1010 pattern at **512 bps** using the selected output mode, then stop.
```
DEBUG_SCOPE
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

### SEND_MIN <capcode> <function 0-3> <preamble_ms>
Send a minimal page burst: preamble (1010) + sync + one batch with only the address codeword.
```
SEND_MIN 123456 0 1500
```

### SEND_MIN_LOOP <capcode> <function 0-3> <preamble_ms> <seconds>
Repeat the minimal page burst until timeout.
```
SEND_MIN_LOOP 123456 0 1500 30
```

### SEND_SYNC
Send a preamble + sync + short idle for scope verification.
```
SEND_SYNC
```

### AUTOTEST <capcode> [seconds]
Sweep invert/idle/function/preamble combinations to brute-force a working page (baud locked to 512).
```
AUTOTEST 123456 120
```
Notes:
- POCSAG maps capcodes to RF addresses via **capcode / 2**, so **123456** and **123457**
  target the same address; the function bits differentiate them.
- AUTOTEST tries invert **0/1**, idle **1/0**, function **0-3**, and preamble lengths
  **576/1152/2304** (baud fixed at **512 bps**).

### AUTOTEST2 <capcode> [seconds]
Sweeps **profiles + baud + invert + idle + function + preamble** and iterates across GPIOs in the
`SET_GPIO_LIST` list (or the single configured GPIO if unset).
```
AUTOTEST2 123456 120
```
Profiles tested:
- `NRZ_SLICED`
- `NRZ_PUSH_PULL`
- `MANCHESTER`
- `DISCRIM_AUDIO_FSK`
- `SLICER_EDGE_PULSE`

### AUTOTEST2 STOP
Stop a running AUTOTEST2 early.
```
AUTOTEST2 STOP
```

### AUTOTEST_FAST <seconds>
Fast deterministic sweep for bring-up (fixed capcode **123456** at **512 bps**, output **OPEN_DRAIN**).
```
AUTOTEST_FAST 60
```

### AUTOTEST STOP
Stop a running AUTOTEST early.
```
AUTOTEST STOP
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

## First power-on test (serial commands)
Run these commands in order on the serial console to validate the DATA pin wiring:
```
STATUS
DEBUG_SCOPE
AUTOTEST_FAST 60
```

## Recommended settings (Advisor II RF DATA injection)
- **BAUD:** 512  
- **INVERT:** 0  
- **OUTPUT:** OPEN_DRAIN  
- **CAPCODES:** 123456 (individual) / 123457 (group)

## Injecting into logic board (no RF board)
When the RF board is removed, inject into the **logic board connector pin that mates with the RF board’s DATA pin** (same DATA net). Wire:
- **GND → logic board GND**
- **DATA → logic board RF connector DATA pin**

## Troubleshooting (top 3)
1. **Wrong pad / net**: confirm you are on the RF board connector **DATA** pin (service manual).
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
  Verify **common ground** and the DATA injection node continuity to the RF board connector **DATA** pin.
- **Probe doesn’t auto-save capcodes**  
  `PROBE START` and `PROBE BINARY` **require ALERT_GPIO**. If ALERT_GPIO isn’t wired, you’ll get `ERROR PROBE NO_ALERT_GPIO`. Use `PROBE ONESHOT` and watch the pager manually.

### Notes
- Leading zeros are just formatting (e.g., **0123456 == 123456**).
