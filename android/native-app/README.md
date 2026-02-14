# PagerBridge Native Forwarder (Android)

Android app that forwards Google Messages notifications to the ESP32 BLE pager bridge.

## What it does
- Monitors notifications from `com.google.android.apps.messaging`.
- Reads sender (`android.title`) and message body (`android.text`).
- Writes BLE command:
  - `SEND <sender>: <message>\n`
- Supports background operation through NotificationListenerService.
- Optional ongoing system notification indicator.
- Provides UI settings for:
  - BLE device name / bonded-device selection
  - BLE device address
  - Service UUID
  - RX characteristic UUID
- Shows message pass count, log preview window, and clear-log action.

## VS Code on macOS build flow

### Prerequisites
- VS Code
- Android Studio (recommended, includes a compatible JDK)
- Android SDK installed (`ANDROID_HOME` / `ANDROID_SDK_ROOT` configured)
- USB debugging enabled on Android device (for install)

### Build
From repo root:

```zsh
cd android/native-app
./gradlew assembleDebug
```

`./gradlew` in this repo is a local bootstrap script that:
- detects Android Studio JDK (`/Applications/Android Studio.app/Contents/jbr/Contents/Home`)
- detects Android SDK (`~/Library/Android/sdk`)
- keeps Gradle cache in `android/native-app/.gradle-user-home`

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
5. Save desired BLE settings (or choose a paired BLE device from list).

## Notes / limitations
- Uses notification contents (not direct SMS provider reads).
- If notification content is hidden/private, forwarding content may be unavailable.
