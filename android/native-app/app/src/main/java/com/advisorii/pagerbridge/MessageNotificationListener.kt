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
        val pagerMessage = "$sender: $sanitizedBody"
        val boundedPagerMessage = if (pagerMessage.length <= MAX_PAGER_MESSAGE_CHARS) {
            pagerMessage
        } else {
            pagerMessage.take(MAX_PAGER_MESSAGE_CHARS - 3) + "..."
        }
        val outbound = "SEND $boundedPagerMessage\n"
        BlePagerClient.sendToPager(this, outbound) { result ->
            val ts = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date())
            val response = result.statusResponse.orEmpty().trim()
            val detail = if (response.isBlank()) result.message else response.replace("\n", " | ")
            val normalized = response.lowercase(Locale.US)
            val queued = result.success && normalized.contains("send queued")
            val resultTag = when {
                queued -> "queued"
                result.success && normalized.contains("send queue busy") -> "queue_busy"
                result.success -> "ack_only"
                else -> "failed"
            }
            BridgePreferences.appendLog(this, "[$ts] $resultTag | $sender: $preview | $detail")
            if (queued) {
                BridgePreferences.incrementPassCount(this)
            }
            BridgeForegroundService.sync(this)
        }
    }

    companion object {
        private const val GOOGLE_MESSAGES_PACKAGE = "com.google.android.apps.messaging"
        private const val MAX_PAGER_MESSAGE_CHARS = 200
    }
}
