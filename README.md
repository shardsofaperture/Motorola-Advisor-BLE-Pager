# Advisor II BLE Receiver (POCSAG NRZ Simplified)

This firmware outputs a deterministic POCSAG NRZ stream on an ESP32-S3 GPIO for scope validation
and pager injection. Configuration is stored in LittleFS at `/config.json`.

## Quick start
### Wiring
- **DATA → pager pin 4**
- **GND → pager GND** (common ground)

### First boot
On first boot the device prints **HELP** automatically so the README is optional.

### Recommended bring-up order
1. `SCOPE 2000` (confirm edges)
2. `ADDR 10 IND` (prove address-only alert)
3. `T1 10` (try `HELLO WORLD`)

## Commands (Serial @ 115200)
- `STATUS`
- `SET <key> <value>`
- `SAVE` / `LOAD`
- `H` (send one "H")
- `T1 <seconds>` (repeat `HELLO WORLD`)
- `ADDR <seconds> [IND|GRP|BOTH]` (address-only burst loop)
- `SCOPE <ms>`
- `HELP` / `?`

### SET keys
- `baud` (512/1200/2400)
- `invert`
- `idleHigh`
- `output` (`open_drain` / `push_pull`)
- `preambleMs`
- `capInd`
- `capGrp`
- `functionBits`
- `dataGpio`
- `frameGapMs`
- `repeatPreambleEachFrame`

## Build & flash (PlatformIO)
```bash
platformio run -t upload
platformio device monitor
```
