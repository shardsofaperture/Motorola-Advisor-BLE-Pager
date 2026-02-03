# Advisor II BLE Receiver (POCSAG NRZ Simplified)

This firmware outputs a deterministic POCSAG NRZ stream on an ESP32-S3 GPIO for scope validation
and pager injection. The build keeps Arduino/PlatformIO and uses LittleFS for config storage.

## Quick Start
### Wiring
- **XIAO GND → pager GND** (common ground required; battery negative is fine).
- **XIAO D3 (GPIO4) → pager data-in** through a **1k–2k2 series resistor**.

### Serial usage (115200)
- `STATUS`
- `SCOPE 2000`
- `T1 10`
- `H`
- `SAVE`

## Wiring notes
- The pager data-in pin is **post-slicer, pre-ASIC** and expects open-collector style drive.

### Drive modes
- **open_drain (default)**
  - Logic 1 / idle: **Hi-Z** (pinMode INPUT, no pullups)
  - Logic 0: **drive LOW** (pinMode OUTPUT, LOW)
- **push_pull**
  - Logic 1 / idle: **drive HIGH**
  - Logic 0: **drive LOW**

## Config file
The device uses LittleFS with `/config.json`. On first boot, it creates the file with defaults.

```json
{
  "baud": 512,
  "invert": false,
  "idleHigh": true,
  "output": "open_drain",
  "preambleMs": 2000,
  "odPullup": false,
  "bootPreset": "pager",
  "capInd": 1422890,
  "capGrp": 1422890,
  "functionBits": 0,
  "dataGpio": 4
}
```

**Field notes**
- `baud`: 512, 1200, or 2400.
- `invert`: invert NRZ bits before output.
- `idleHigh`: logical idle state.
- `output`: `open_drain` or `push_pull`.
- `preambleMs`: preamble duration (1010...)
- `odPullup`: when `output` is `open_drain`, drive idle/high as `INPUT_PULLUP` if true.
- `bootPreset`: preset name to apply at boot (`pager`, `bench`, `scope`, `invert`).
- `capInd` / `capGrp`: capcodes (no auto +1).
- `functionBits`: 0–3.
- `dataGpio`: output pin.

## Default settings
- `baud`: 512
- `invert`: false
- `idleHigh`: true
- `output`: open_drain
- `preambleMs`: 2000
- `capInd`: 1422890
- `capGrp`: 1422890
- `functionBits`: 0
- `dataGpio`: GPIO4 (XIAO D3)

## Commands (Serial @ 115200)
POCSAG TX ready.
Data pin: GPIO4 (XIAO D3). baud=512 invert=false idleHigh=true output=open_drain preambleMs=2000
Commands:
  STATUS                 - show current settings
  SET <k> <v>            - change setting
  SAVE / LOAD            - persist/restore config.json
  H                      - send one test page "H" to capInd
  T1 <sec>               - loop HELLO WORLD for <sec>
  SCOPE <ms>             - output 1010 pattern for scope
  HELP or ?              - show this menu
Examples:
  STATUS
  SET baud 512
  SET invert true
  SCOPE 2000
  T1 10
  H
  SAVE

## Build & flash (PlatformIO)
```bash
platformio run -t upload
platformio device monitor
```
