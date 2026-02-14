package com.advisorii.pagerbridge

import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification

class MessageNotificationListener : NotificationListenerService() {

    override fun onNotificationPosted(sbn: StatusBarNotification) {
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
        val outbound = "SEND $sender: $sanitizedBody\n"
        BlePagerClient.sendToPager(this, outbound)
    }

    companion object {
        private const val GOOGLE_MESSAGES_PACKAGE = "com.google.android.apps.messaging"
    }
}
