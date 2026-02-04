# Advisor II BLE Receiver (POCSAG NRZ Simplified)

Deterministic POCSAG NRZ transmitter for ESP32-S3 using hardware-timed RMT output.
Configuration lives in LittleFS at `/config.json`.

## How to wire
- ESP32 GND → pager battery negative / ground
- XIAO ESP32S3 **D3** (GPIO4) → series resistor (start 1k–2.2k; 470 Ω OK) → pager header pin 4
- D3 on the XIAO ESP32S3 maps to GPIO4.
- Keep the pager’s own pull-up/biasing. Start with `output open_drain`; if no decode, try
  `output push_pull`.

## Quick start
1. Flash the firmware.
2. Open Serial Monitor @ 115200.
3. `PRESET ADVISOR`
4. `STATUS`
5. `SCOPE 2000` (verify timing)
6. `H`
7. Known-good baseline: `PRESET ADVISOR` then `H`.

## If scope looks good but pager still doesn’t decode
- Toggle `SET output open_drain` vs `SET output push_pull`.
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
