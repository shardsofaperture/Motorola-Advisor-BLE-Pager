# Troubleshooting

## 1) BLE connect fails

Checklist:
1. Confirm the pager bridge is powered and advertising as **`PagerBridge`**.
2. Verify Android BLE permissions for Tasker + BLE plugin:
   - Nearby devices / Bluetooth
   - Location (if required by device/OEM)
3. Confirm plugin UUID configuration matches firmware exactly:
   - Service: `1b0ee9b4-e833-5a9e-354c-7e2d486b2b7f`
   - RX write: `1b0ee9b4-e833-5a9e-354c-7e2d496b2b7f`
   - Status notify: `1b0ee9b4-e833-5a9e-354c-7e2d4a6b2b7f`
4. If name-based connect is flaky, set `%BLE_DEVICE` to a fixed MAC and retry.
5. Remove battery optimization for Tasker + BLE plugin and reboot phone after permission changes.

## 2) Messages are not captured

Checklist:
1. Verify AutoNotification notification listener is enabled.
2. Confirm intercept filters include the right app packages:
   - `com.google.android.apps.messaging`
   - your device SMS package (e.g. `com.android.mms`, OEM messenger)
3. Ensure profile is excluding persistent/system notifications only, not all notifications.
4. Send a test SMS and inspect `%antitle`/`%antext` in Tasker run log.
5. If title/body fields are missing for a specific app build, enable `%USE_AUTOINPUT=1` and use constrained package/activity/text selectors in fallback profile.

## 3) Duplicate forwards

Checklist:
1. Confirm `%LAST_MSG_HASH` updates only when a message is accepted for forward.
2. Ensure dedupe compares `%MSG_HASH` (`package|title|text`) before BLE send.
3. Tighten intercept filters (ignore summary/grouping updates from notification manager).
4. Add short cooldown (500-1500 ms) if your OEM repeatedly updates same notification card.
5. Validate that AutoInput fallback is off (`%USE_AUTOINPUT=0`) unless needed, to avoid duplicate capture pipelines.

## 4) Connected but no response (`PONG` absent)

Checklist:
1. Confirm write payload includes newline terminator (`PING\n`).
2. Verify status characteristic subscription is active before ping write.
3. Test manually with nRF Connect using same service/characteristics.
4. Check firmware serial logs for BLE writes/notifications.
5. Temporarily reduce retries/timeouts to surface a deterministic failure pattern in Tasker logs.
