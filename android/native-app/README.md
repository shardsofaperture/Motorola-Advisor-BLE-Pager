# PagerBridge Android App

Android app that forwards Google Messages notifications to the ESP32 BLE pager bridge firmware.

## Current behavior

1. Listens for notifications from `com.google.android.apps.messaging`.
2. Extracts sender (`android.title`) and message body (`android.text`).
3. Sends BLE payload:
   - `SEND <sender>: <message>\n`
4. Tracks total forwarded count.
5. Uses short-lived log entries that auto-expire after about 10 seconds (not persisted).
6. Uses optional ongoing foreground notification; tapping it opens `MainActivity`.

## Build

```zsh
cd android/native-app
./gradlew assembleDebug
```

APK:
- `app/build/outputs/apk/debug/app-debug.apk`

Install:

```zsh
cd android/native-app
./gradlew installDebug
```

## First-run setup

1. Open app.
2. Grant Bluetooth permissions.
3. Grant notification permission (Android 13+).
4. Enable notification access for this app.
5. Set BLE device name/address and UUIDs to match firmware.

## BLE settings defaults

- Device name: `PagerBridge`
- Service UUID: `1b0ee9b4-e833-5a9e-354c-7e2d486b2b7f`
- RX UUID: `1b0ee9b4-e833-5a9e-354c-7e2d496b2b7f`

## Limitation

- "Queued" in app logs means BLE connect/write flow was initiated; it is not a strict end-to-end pager ACK.
