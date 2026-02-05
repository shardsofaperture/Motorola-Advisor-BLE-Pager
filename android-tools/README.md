# Android Automation Assets

This directory contains Tasker automation assets for forwarding Android message notifications to the `PagerBridge` BLE receiver.

## Required Apps / Plugins
- **Tasker** (automation engine)
- **AutoNotification** (notification intercept for SMS + Google Messages/RCS)
- **AutoInput** (optional fallback when notification payload fields are incomplete)
- **BLE Tasker Plugin**: `nl.steinov.bletaskerplugin`

## BLE Target Values
Use these values exactly as defined in the firmware repository README:
- Device name: `PagerBridge`
- Service UUID: `1b0ee9b4-e833-5a9e-354c-7e2d486b2b7f`
- RX UUID (write): `1b0ee9b4-e833-5a9e-354c-7e2d496b2b7f`
- Status UUID (notify): `1b0ee9b4-e833-5a9e-354c-7e2d4a6b2b7f`

## Android Permissions / Power Settings
Configure these for reliable background delivery:

1. **Tasker**
   - Notification access (for AutoNotification trigger path)
   - Run in background / unrestricted battery mode
   - Disable battery optimization
   - Allow autostart (OEM-specific)

2. **AutoNotification**
   - Notification listener access enabled
   - Background activity allowed
   - Disable battery optimization

3. **BLE Tasker Plugin (`nl.steinov.bletaskerplugin`)**
   - Nearby devices / Bluetooth permissions
   - Location permission where required by Android version/OEM BLE stack
   - Disable battery optimization

4. **AutoInput** (optional fallback profile only)
   - Accessibility service enabled
   - Display over other apps (if requested by workflow)
   - Disable battery optimization

## Import Instructions
1. Copy `android-tools/tasker/SmstoPager.prj.xml` to your phone.
2. In Tasker, open **Data > Restore** and import the project XML.
3. Install/enable all required plugins and permissions above.
4. Review globals in `android-tools/tasker/variables.md`.
5. Validate end-to-end with a test message and BLE `PONG` check.

## Files in this Folder
- `tasker/SmstoPager.prj.xml`: Tasker project export containing profiles + tasks.
- `tasker/variables.md`: Variable reference (globals and locals).
- `tasker/troubleshooting.md`: BLE and notification debugging checklist.
