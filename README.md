# Advisor II BLE Receiver (POCSAG NRZ Simplified)

Deterministic POCSAG NRZ transmitter for ESP32-S3 using hardware-timed RMT output.
Configuration lives in LittleFS at `/config.json`.

## How to wire
- ESP32 GND → pager battery negative / ground
- ESP32 GPIO (XIAO ESP32S3 **D3** / GPIO4) → 480 Ω series resistor → pager header pin 4
- D3 on the XIAO ESP32S3 maps to GPIO4.
- Share ground between the ESP32 and pager.
- Default output mode is `push_pull` for reliable paging.

## Quick start
1. Flash the firmware.
2. Open Serial Monitor @ 115200.
3. Run the exact commands below to reproduce the paging path:
   - `STATUS`
   - `DEBUG_SCOPE`
   - `SEND_MIN 1422890 0`

Defaults (ADVISOR preset): baud=512, invertWords=false, driveOneLow=true, idleHigh=true,
output=push_pull.

## If scope looks good but pager still doesn’t decode
- Toggle `SET output open_drain` vs `SET output push_pull` (open-drain needs an external pull-up).
- Toggle `SET driveOneLow true/false`.
- Toggle `SET invertWords true/false`.
- Try `SET preambleBits 576` vs `SET preambleBits 1024`.
- Verify you are on header pin 4 (not 7) and ground is solid.
- Confirm XIAO D3 is GPIO4; if using another pin, `SET dataGpio <pin>`.

## Commands (Serial @ 115200)
- `STATUS`
- `PRESET ADVISOR|GENERIC`
- `SET <key> <value>`
- `SAVE` / `LOAD`
- `H` (send one "H")
- `SEND <text>` (send arbitrary text)
- `T1 <seconds>` (repeat `HELLO WORLD`)
- `SCOPE <ms>`
- `DEBUG_SCOPE`
- `SEND_MIN <capcode> <func>`
- `HELP` / `?`

### SET keys
- `baud` (512/1200/2400)
- `preambleBits`
- `capInd`
- `capGrp`
- `functionBits`
- `dataGpio`
- `output` (`open_drain` / `push_pull`)
- `invertWords`
- `driveOneLow`
- `idleHigh`
