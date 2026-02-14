package com.advisorii.pagerbridge

import android.content.ComponentName
import android.content.Context
import android.provider.Settings

object NotificationAccessState {
    fun isNotificationListenerEnabled(context: Context): Boolean {
        val enabledListeners = Settings.Secure.getString(
            context.contentResolver,
            "enabled_notification_listeners"
        ) ?: return false

        val componentName = ComponentName(context, MessageNotificationListener::class.java)
        return enabledListeners.split(':').any { it.equals(componentName.flattenToString(), true) }
    }
}
