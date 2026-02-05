# Tasker Variable Reference

## Globals

| Variable | Purpose | Lifecycle |
|---|---|---|
| `%BLE_DEVICE` | Preferred BLE target identifier (`PagerBridge` name or fixed MAC) used by BLE plugin actions. | Set by `PagerBLE_SetDefaults`; persistent global. |
| `%PAGER_LOCK` | Mutex-style lock to prevent concurrent BLE connection attempts from overlapping task triggers. | Set to `1` on entry to `PagerBLE_EnsureConnected`, always cleared to `0` before exit. |
| `%CONNECTED` | Current connection/health state (`1` connected, `0` not ready). | Reset at each connect sequence; set after successful BLE connect + ping/notify verification. |
| `%LAST_MSG_HASH` | Last forwarded message key for short-term dedupe (built from package/title/text). | Updated when a message is accepted for forwarding; compared on each new intercept event. |
| `%USE_AUTOINPUT` | Toggle for optional UI-capture fallback profile (`1` enabled, `0` disabled). | Operator-controlled global; default `0`. |

## Local / Per-run Variables

| Variable | Purpose | Lifecycle |
|---|---|---|
| `%antitle` | AutoNotification title field (typically sender/conversation title). | Populated by AutoNotification Intercept event each trigger. |
| `%antext` | AutoNotification body text field (message preview content). | Populated by AutoNotification Intercept event each trigger. |
| `%anpackage` | Source app package from notification event for app filtering. | Populated per notification event. |
| `%anpersistent` | Notification persistency signal used to ignore ongoing/system noise. | Populated per notification event. |
| `%RAW_MSG` | Unnormalized outbound command template: `SEND %antitle: %antext`. | Built inside `SmstoPager_Core`, then transformed. |
| `%PAYLOAD` | Final bounded payload sent over BLE (newline added at write step). | Built inside `SmstoPager_Core`; max length enforced before send. |
| `%MSG_HASH` | Current event dedupe key (`%anpackage|%antitle|%antext`). | Recomputed each run; compared with `%LAST_MSG_HASH`. |
| `%BREAK` | Early-exit guard flag when message is empty, duplicate, or filtered out. | Set in filter checks, consumed in same run only. |
| `%TRIES` | Retry loop counter for BLE connection attempts. | Initialized to `0`; incremented inside bounded loop in `PagerBLE_EnsureConnected`. |
| `%bt_last_notify` | Latest BLE notify payload from status characteristic (plugin output variable). | Set by BLE plugin after status subscribe/notify events (e.g., `PONG`). |
| `%ai_title` / `%ai_text` | AutoInput query result placeholders for optional fallback capture path. | Used only when `%USE_AUTOINPUT=1`. |

## Recommended Defaults
- `%BLE_DEVICE=PagerBridge` (or set to stable BLE MAC for faster/reliable reconnects).
- `%USE_AUTOINPUT=0` until notification capture is verified incomplete for a specific app.
