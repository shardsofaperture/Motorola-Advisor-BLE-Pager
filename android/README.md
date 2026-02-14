# Android App

This directory contains the Android app source used to forward Google Messages notifications to the `PagerBridge` BLE receiver.

## Native app capabilities
- Listens for **Google Messages** notifications (`com.google.android.apps.messaging`).
- Extracts sender + body text from posted notifications.
- Writes `SEND <sender>: <message>\n` over BLE to PagerBridge.
- Runs in the background via Android notification listener service.
- Optional ongoing status notification in the system shade.
- On-device settings for target BLE name/address and service/characteristic UUIDs.
- Message pass counter and in-app log preview window.

## BLE Target defaults
- Device name: `PagerBridge`
- Service UUID: `1b0ee9b4-e833-5a9e-354c-7e2d486b2b7f`
- RX UUID (write): `1b0ee9b4-e833-5a9e-354c-7e2d496b2b7f`
- Status UUID (notify): `1b0ee9b4-e833-5a9e-354c-7e2d4a6b2b7f`

## Files in this folder
- `native-app/`: Android app project (Kotlin + Gradle).
