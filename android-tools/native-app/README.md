# PagerBridge Native Forwarder (Android)

Minimal Android app that replaces Tasker + AutoNotification for forwarding Google Messages notifications to the ESP32 BLE pager bridge.

## What it does
- Monitors notifications from `com.google.android.apps.messaging`.
- Reads sender (`android.title`) and message body (`android.text`).
- Writes a BLE command in the format:
  - `SEND <sender>: <message>\n`
- Targets device name `PagerBridge` and writes to RX characteristic UUID:
  - `1b0ee9b4-e833-5a9e-354c-7e2d496b2b7f`

## Requirements
- Android Studio Iguana+ (or recent Gradle/AGP compatible version).
- Android 8.0+ device (API 26+).
- Pager ESP32 powered and advertising as `PagerBridge`.
- Pager device should be paired/bonded once in Android Bluetooth settings.

## Build
1. Open `android-tools/native-app/` in Android Studio.
2. Let Gradle sync.
3. Build/install `app` to your phone.

## First-run setup on phone
1. Open app.
2. Tap **Grant Bluetooth Permissions**.
3. Tap **Grant Notification Runtime Permission** (Android 13+).
4. Tap **Open Notification Access Settings** and enable `PagerBridge Forwarder`.

## Notes / limitations
- This MVP relies on notification contents, not direct SMS provider reads.
- For RCS/Google Messages this is the most practical no-Tasker route.
- If the message app hides text in notifications (private mode), content may be unavailable.
