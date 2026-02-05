# Advisor II BLE Receiver (POCSAG NRZ Simplified)

Deterministic POCSAG NRZ transmitter for ESP32-S3 using hardware-timed RMT output.
Configuration lives in LittleFS at `/config.json`.

## BLE GATT + nRF Connect quick test
- Device name: `PagerBridge`
- Service UUID: `1b0ee9b4-e833-5a9e-354c-7e2d486b2b7f`
- RX (write) characteristic UUID: `1b0ee9b4-e833-5a9e-354c-7e2d496b2b7f`
- Status (read/notify) characteristic UUID: `1b0ee9b4-e833-5a9e-354c-7e2d4a6b2b7f`

Test flow (nRF Connect):
1. Connect to `PagerBridge`.
2. Write `PING` to the RX characteristic.
3. Observe a status notify with `PONG\n`.
4. Write `SEND 123456 0 test message` to the RX characteristic.
5. Observe `TX_DONE` or `TX_FAIL` status notifications.

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

## Bluetooth (Android/Tasker)
- Device name: `PagerBridge` (BLE GATT server advertises on boot).
- Write commands by sending a full line ending in newline, e.g. `SEND HELLO\n`.
- RX (write) characteristic UUID: `1b0ee9b4-e833-5a9e-354c-7e2d496b2b7f`
- STATUS (read/notify) characteristic UUID: `1b0ee9b4-e833-5a9e-354c-7e2d4a6b2b7f`
- Service UUID: `1b0ee9b4-e833-5a9e-354c-7e2d486b2b7f`
- Use `PING` to receive `PONG\n` on the status characteristic.

## Android automation assets
Android automation assets live in `android-tools/`.

- Import the Tasker project export: `android-tools/tasker/SmstoPager.prj.xml`.
- Required plugins/apps are documented in `android-tools/README.md`.
- Variable definitions and debugging steps are in `android-tools/tasker/variables.md` and `android-tools/tasker/troubleshooting.md`.
