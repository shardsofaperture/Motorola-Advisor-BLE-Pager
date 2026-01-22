# PagerBridge (ESP32-S3 → Motorola Advisor II DATA IN)

This project implements a BLE-to-pager bridge for the Seeed XIAO ESP32-S3. An Android phone writes SMS text over BLE, and the ESP32-S3 injects a POCSAG bitstream into the Motorola Advisor II pager’s internal `DATA IN` pin (no RF).

## Hardware wiring

- **ESP32 GND** → Pager **GND** (pin 6)
- **ESP32 GPIO4** → **1k series resistor** → Pager **DATA IN** (pin 1)
- Pager remains on its AA battery. ESP32 is powered separately (LiPo to XIAO BAT).

## Build & flash

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```

## BLE interface

- **Device name:** `PagerBridge`
- **Service UUID:** `7f2b6b48-2d7e-4c35-9e5a-33e8b4e90e1b`
- **RX characteristic (write):** `7f2b6b49-2d7e-4c35-9e5a-33e8b4e90e1b`
- **Status characteristic (read/notify):** `7f2b6b4a-2d7e-4c35-9e5a-33e8b4e90e1b`

### Payload formats

- Raw text: `Hello pager`
- Labeled format:
  ```
  FROM: Alice
  MSG: Hello
  ```
  The firmware will combine this into `FROM:Alice MSG:Hello`.

### Commands

Send these commands via the RX characteristic:

- `!rate=512` / `!rate=1200` / `!rate=2400`
- `!cap=1234567` (sets capcode)
- `!test=1` (emit 1010 test pattern)
- `!alert=0` or `!alert=1` (alert flag placeholder)
- `!learn` (placeholder hook, not implemented)

## Test with logic analyzer

1. Connect a logic analyzer to GPIO4.
2. Send `!test=1` to transmit a 1010 pattern.
3. Send a normal text message and observe a POCSAG preamble followed by sync + codewords.

## Implementation notes

- BLE RX → FreeRTOS queue → POCSAG encoder → RMT playback → status notify.
- The encoder produces a standard POCSAG batch: preamble, sync, address, message, idle fill.
- The DATA IN pin idles LOW.

## Defaults

- Capcode: `1234567`
- Bitrate: `512`
- Alert: enabled

## Extending

- **Learn mode**: add a handler for `!learn` to capture capcode or frames.
- **Deep sleep**: add a GPIO wake button and advertise for a limited window.
- **Alert modes**: map alert flags into POCSAG function bits or message content.
