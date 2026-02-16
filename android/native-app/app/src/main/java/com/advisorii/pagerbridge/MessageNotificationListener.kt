package com.advisorii.pagerbridge

import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class MessageNotificationListener : NotificationListenerService() {

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        val config = BridgePreferences.loadConfig(this)
        if (!config.forwardingEnabled) {
            return
        }

        if (sbn.packageName != GOOGLE_MESSAGES_PACKAGE) {
            return
        }

        val extras = sbn.notification.extras
        val sender = extras.getCharSequence("android.title")?.toString()?.trim().orEmpty()
        val body = extras.getCharSequence("android.text")?.toString()?.trim().orEmpty()

        if (sender.isBlank() || body.isBlank()) {
            return
        }

        val sanitizedBody = body.replace("\n", " ")
        val preview = sanitizedBody.take(80)
        val outbound = "SEND $sender: $sanitizedBody\n"
        val sent = BlePagerClient.sendToPager(this, outbound)

        val ts = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date())
        val result = if (sent) "queued" else "failed"
        BridgePreferences.appendLog(this, "[$ts] $result | $sender: $preview")
        BridgePreferences.incrementPassCount(this)
        BridgeForegroundService.sync(this)
    }

    companion object {
        private const val GOOGLE_MESSAGES_PACKAGE = "com.google.android.apps.messaging"
    }
}
