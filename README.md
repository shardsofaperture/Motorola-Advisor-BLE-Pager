# Advisor II BLE Receiver (POCSAG NRZ Simplified)

Deterministic POCSAG NRZ transmitter for ESP32-S3. Configuration lives in LittleFS at
`/config.json`.

## Wiring
- Pager pin 4 → series resistor → XIAO ESP32-S3 **D3** (GPIO4)
- Common ground between pager and XIAO

## Recommended bring-up
1. `PRESET lora_baseline`
2. `SCOPE 2000`
3. `H`
4. `T1 10`
5. If needed, toggle `SET invert true/false`, `SET alphaBitOrder lsb_first/msb_first`,
   and `SET baudScale 0.991` (or nearby).

## Commands (Serial @ 115200)
- `STATUS`
- `PRESET pager|lora_baseline|fast1200`
- `SET <key> <value>`
- `SAVE` / `LOAD`
- `H` (send one "H")
- `T1 <seconds>` (repeat `HELLO WORLD`)
- `ADDR <seconds> [IND|GRP|BOTH]` (address-only burst loop)
- `SCOPE <ms>`
- `DIAG <msg> [capcode]`
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
- `baudScale` (0.90–1.10)
- `alphaBitOrder` (`lsb_first` / `msb_first`)
- `frameGapMs` (0–1000)
- `repeatPreambleEachFrame`
