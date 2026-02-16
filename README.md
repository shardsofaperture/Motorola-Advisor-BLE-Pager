# Advisor BLE Receiver (Motorola Advisor / Linguist)

This project bridges Android notifications to an original Motorola Advisor pager through a XIAO ESP32S3.

It includes:
1. ESP32 firmware (`ESP-IDF` in PlatformIO) that receives BLE writes and transmits POCSAG data to the pager.
2. Android app (`android/native-app`) that listens to Google Messages notifications and forwards payloads over BLE.

This repository now targets the original Motorola Advisor (Linguist use case).

## Architecture

1. Android receives an SMS notification from Google Messages.
2. Android app formats `SEND <sender>: <message>\n` and writes it to BLE RX characteristic.
3. ESP32 parses command, builds POCSAG words, and drives pager data line via GPIO.
4. ESP32 keeps BLE available with low-power advertising profile and PM settings.

## Repository layout

- `src/main.cpp`: active firmware source
- `platformio.ini`: PlatformIO build/upload/monitor config
- `sdkconfig.defaults`, `sdkconfig.xiao_esp32s3_espidf`: ESP-IDF options
- `huge_app.csv`: partition table
- `android/native-app/`: Android application

## Hardware

- Board: Seeed XIAO ESP32S3
- Pager data pin: GPIO4 (XIAO D3 in this project)
- Typical wiring:
1. ESP32 GND -> pager GND (common ground required)
2. GPIO4 -> series resistor (e.g. ~480 ohm) -> pager data input

## BLE profile

- Device name: `PagerBridge`
- Service UUID: `1b0ee9b4-e833-5a9e-354c-7e2d486b2b7f`
- RX characteristic (write): `1b0ee9b4-e833-5a9e-354c-7e2d496b2b7f`
- Status characteristic (read/notify): `1b0ee9b4-e833-5a9e-354c-7e2d4a6b2b7f`

## Firmware behavior

- Default POCSAG config:
1. capcode `1422890`
2. function bits `2`
3. baud `512`
4. preamble bits `576`
- LED behavior:
1. on for first 10 seconds at boot
2. short heartbeat blink every 15 seconds
- Power behavior:
1. PM arms 10 seconds after boot
2. DFS configured to 40-80 MHz (`light_sleep` disabled)
3. Fast reconnect advertising window (200-300 ms for 15s), then slow idle advertising (2.0-3.0s)
- Runtime BLE TX power is adjustable with command (`txpower <dbm>`)

## Serial/BLE command interface

Commands accepted on serial monitor and BLE RX:

- `send <message>`: enqueue pager message
- `status`: POCSAG + GPIO + BLE state summary
- `pm`: PM configuration state
- `pm locks`: active PM lock dump (debug power blockers)
- `metrics`: uptime/connected/advertising/cpu frequency/load metrics
- `txpower`: show current target + active BLE TX levels
- `txpower <dbm>`: set TX power; allowed `-24,-21,-18,-15,-12,-9,-6,-3,0,3,6,9,12,15,18,20`
- `ble`: BLE status (interval/profile/MAC/UUIDs/tx power)
- `ble restart`: restart advertising if disconnected
- `ping`: response check
- `reboot`: soft reboot
- `help`: command summary

## Build and flash firmware (PlatformIO)

Environment is `xiao_esp32s3_espidf`.

1. Build:
```zsh
pio run --environment xiao_esp32s3_espidf
```

2. Flash:
```zsh
pio run --environment xiao_esp32s3_espidf --target upload
```

3. Serial monitor:
```zsh
pio device monitor -p /dev/cu.usbmodem14401 -b 115200 --echo --eol LF
```

## Android app

Source: `android/native-app`

What it does:
1. Notification listener watches `com.google.android.apps.messaging`
2. Extracts sender + body from notification extras
3. Writes BLE payload to pager bridge
4. Shows pass count in UI
5. Keeps only short-lived logs (auto-expire after ~10 seconds, non-persistent)
6. Foreground notification tap re-opens the app

### Build APK

```zsh
cd android/native-app
./gradlew assembleDebug
```

Output:
- `android/native-app/app/build/outputs/apk/debug/app-debug.apk`

### Install

```zsh
cd android/native-app
./gradlew installDebug
```

### First run setup

1. Grant Bluetooth permissions
2. Grant notification permission (Android 13+)
3. Enable notification listener access for this app
4. Select bonded BLE device or set address/name manually
5. Verify UUIDs match firmware defaults

## Test checklist

1. Flash firmware and open monitor
2. Run `status`, `pm`, `ble`, `metrics`
3. From phone app, send test notification from Google Messages
4. Confirm monitor shows `Queued:` and `TX_DONE`
5. Confirm pager alerts with expected message

## Known constraints

- `light_sleep` is intentionally off because BLE stability is prioritized on this target build.
- While USB serial monitor is attached, PM lock state differs from battery-only operation.
- Android side currently treats "write started" as queued, not guaranteed end-to-end delivery ACK.

## Future work

- Add a parallel variant of this project for the Motorola Advisor II.
