# PagerBridge Native Forwarder (Android)

Minimal Android app that forwards Google Messages notifications to the ESP32 BLE pager bridge.

## What it does
- Monitors notifications from `com.google.android.apps.messaging`.
- Reads sender (`android.title`) and message body (`android.text`).
- Writes BLE command:
  - `SEND <sender>: <message>\n`
- Targets device name `PagerBridge` using RX characteristic UUID:
  - `1b0ee9b4-e833-5a9e-354c-7e2d496b2b7f`

## VS Code on macOS build flow

### Prerequisites
- VS Code
- JDK 17+
- Android SDK installed (`ANDROID_HOME` / `ANDROID_SDK_ROOT` configured)
- USB debugging enabled on Android device (for install)

### Build
From repo root:

```zsh
cd android/native-app
./gradlew assembleDebug
```

### Install debug build

```zsh
cd android/native-app
./gradlew installDebug
```

## First-run setup on phone
1. Open app.
2. Grant Bluetooth permissions.
3. Grant notification runtime permission (Android 13+).
4. Open notification access settings and enable `PagerBridge Forwarder`.

## Notes / limitations
- Uses notification contents (not direct SMS provider reads).
- For RCS/Google Messages this avoids Tasker plugins.
- If notification content is hidden/private, forwarding content may be unavailable.
