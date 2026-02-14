# Advisor II BLE Receiver

Repository for two deliverables:

1. **ESP32 firmware (with power management)** for the XIAO ESP32S3 pager bridge.
2. **Android app** that forwards Google Messages notifications over BLE.

## Repository layout

- `src/` → **active ESP32 firmware source** (PlatformIO + Arduino + ESP-IDF)
- `platformio.ini` / `sdkconfig.defaults` / `huge_app.csv` → firmware build configuration
- `android/native-app/` → **Android app source** (Gradle Kotlin project)
- `android/tasker/` and `docs/android-tasker.md` → optional Tasker automation assets/docs
- `src_arduino/` → legacy placeholder (not used for builds)

> Only the power-management firmware in `src/` is supported and maintained.

---

## ESP32 firmware (VS Code on macOS)

### Prerequisites
- macOS with **VS Code**
- VS Code extension: **PlatformIO IDE**
- USB data cable for XIAO ESP32S3

### Build and flash
1. Open this repository root in VS Code.
2. Confirm `platformio.ini` default environment is `xiao_esp32s3_idfpm`.
3. Connect the board over USB.
4. In VS Code terminal:
   ```zsh
   pio run
   ```
5. Upload firmware:
   ```zsh
   pio run -t upload
   ```
6. Open serial monitor:
   ```zsh
   pio device monitor -b 115200
   ```

### Quick firmware validation
Use serial commands:
- `STATUS`
- `PING`
- `SEND_MIN 1422890 0`

### BLE GATT constants
- Device name: `PagerBridge`
- Service UUID: `1b0ee9b4-e833-5a9e-354c-7e2d486b2b7f`
- RX (write) characteristic UUID: `1b0ee9b4-e833-5a9e-354c-7e2d496b2b7f`
- Status (read/notify) characteristic UUID: `1b0ee9b4-e833-5a9e-354c-7e2d4a6b2b7f`

### Wiring
- ESP32 GND → pager ground
- XIAO ESP32S3 D3 (GPIO4) → 480 Ω resistor → pager header pin 4
- Ensure common ground between devices

---

## Android app (VS Code on macOS)

The Android source is in `android/native-app/`.

### Prerequisites
- VS Code
- Android Studio (recommended, provides JDK)
- Android SDK + platform tools installed
- Android device with Developer Options + USB debugging enabled

### Build from VS Code terminal
From repo root:

```zsh
cd android/native-app
./gradlew assembleDebug
```

Notes:
- This repo includes a local `android/native-app/gradlew` bootstrap script.
- It auto-detects Android Studio JDK and `~/Library/Android/sdk` on macOS.

APK output:
- `android/native-app/app/build/outputs/apk/debug/app-debug.apk`

### Install to a connected phone
```zsh
cd android/native-app
./gradlew installDebug
```

### First-run phone setup
1. Open the installed app.
2. Grant Bluetooth permissions.
3. Grant notification runtime permission (Android 13+).
4. Enable notification access for the app.

---

## Operational flow

1. Power and flash the ESP32 firmware.
2. Confirm BLE advertising as `PagerBridge`.
3. Install and configure Android app permissions.
4. Send a test SMS / Google Messages notification.
5. Verify firmware receives and transmits page payload.

If needed, use nRF Connect to manually test BLE writes with:

```text
PING\n
SEND 123456 0 test message\n
```

---

## Optional Tasker path

If you prefer Tasker automation instead of the native app, use:
- `android/tasker/SmstoPager.prj.xml`
- `docs/android-tasker.md`
