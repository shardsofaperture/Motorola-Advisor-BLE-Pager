# Advisor II BLE Receiver (POCSAG NRZ Simplified)

This firmware outputs a deterministic POCSAG NRZ stream on an ESP32-S3 GPIO for scope validation
and pager injection. The build keeps Arduino/PlatformIO and uses LittleFS for config storage.

## Wiring
- **GND → pager GND** (common ground required).
- **GPIO (default 3/D2) → RF board DATA pin** with a **1k–2k series resistor**.
- The DATA pin is **post-slicer, pre-ASIC** and expects open-collector style drive.

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
  "dataGpio": 3,
  "rfSenseGpio": -1
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
- `rfSenseGpio`: optional input; set `-1` to disable.

## Commands (Serial @ 115200)
All commands are plain text.

- `STATUS`
  - Print current config in one line.
- `SET <key> <value>`
  - Update RAM config only. Keys match the JSON fields.
- `PRESET <name>`
  - Apply preset immediately. Names: `pager`, `bench`, `scope`, `invert`.
- `SAVE`
  - Write `/config.json`.
- `LOAD`
  - Reload `/config.json`.
- `H`
  - One-shot page with message `H` using current settings.
- `T1 <seconds>`
  - Deterministic test loop: repeatedly sends `HELLO WORLD` every 250 ms for `<seconds>`.
- `SCOPE <ms>`
  - Outputs alternating `1010...` for `<ms>` using current baud/output/invert/idle.
- `RFSENSE`
  - Prints: `count`, `lastSeenMsAgo`, `avgPeriodMs` (if `rfSenseGpio >= 0`).

Examples:
- `PRESET scope; SCOPE 2000`
- `PRESET bench; H`
- `SET bootPreset bench; SAVE`

## Build & flash (PlatformIO)
```bash
platformio run -t upload
platformio device monitor
```
