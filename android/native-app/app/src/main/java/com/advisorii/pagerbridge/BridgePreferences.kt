package com.advisorii.pagerbridge

import android.content.Context

object BridgePreferences {
    private const val PREFS_NAME = "pager_bridge"
    private const val KEY_DEVICE_NAME = "device_name"
    private const val KEY_DEVICE_ADDRESS = "device_address"
    private const val KEY_SERVICE_UUID = "service_uuid"
    private const val KEY_RX_UUID = "rx_uuid"
    private const val KEY_ONGOING_NOTIFICATION = "ongoing_notification"
    private const val KEY_PASS_COUNT = "pass_count"
    private const val KEY_LOGS = "logs"

    private const val LOG_RETENTION_MS = 10_000L
    private const val MAX_LOG_ENTRIES = 80
    private val logLock = Any()
    private val transientLogs = mutableListOf<LogEntry>()

    private data class LogEntry(
        val tsMs: Long,
        val line: String
    )

    data class PagerConfig(
        val deviceName: String,
        val deviceAddress: String,
        val serviceUuid: String,
        val rxUuid: String,
        val ongoingNotification: Boolean
    )

    fun loadConfig(context: Context): PagerConfig {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        return PagerConfig(
            deviceName = prefs.getString(KEY_DEVICE_NAME, "PagerBridge").orEmpty(),
            deviceAddress = prefs.getString(KEY_DEVICE_ADDRESS, "").orEmpty(),
            serviceUuid = prefs.getString(KEY_SERVICE_UUID, "1b0ee9b4-e833-5a9e-354c-7e2d486b2b7f").orEmpty(),
            rxUuid = prefs.getString(KEY_RX_UUID, "1b0ee9b4-e833-5a9e-354c-7e2d496b2b7f").orEmpty(),
            ongoingNotification = prefs.getBoolean(KEY_ONGOING_NOTIFICATION, true)
        )
    }

    fun saveConfig(context: Context, config: PagerConfig) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY_DEVICE_NAME, config.deviceName.trim())
            .putString(KEY_DEVICE_ADDRESS, config.deviceAddress.trim())
            .putString(KEY_SERVICE_UUID, config.serviceUuid.trim())
            .putString(KEY_RX_UUID, config.rxUuid.trim())
            .putBoolean(KEY_ONGOING_NOTIFICATION, config.ongoingNotification)
            .apply()
    }

    fun incrementPassCount(context: Context): Int {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val next = prefs.getInt(KEY_PASS_COUNT, 0) + 1
        prefs.edit().putInt(KEY_PASS_COUNT, next).apply()
        return next
    }

    fun getPassCount(context: Context): Int {
        return context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).getInt(KEY_PASS_COUNT, 0)
    }

    fun appendLog(context: Context, line: String) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .remove(KEY_LOGS)
            .apply()
        val now = System.currentTimeMillis()
        synchronized(logLock) {
            pruneExpiredLogsLocked(now)
            transientLogs += LogEntry(now, line)
            while (transientLogs.size > MAX_LOG_ENTRIES) {
                transientLogs.removeAt(0)
            }
        }
    }

    fun getLogs(context: Context): String {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .remove(KEY_LOGS)
            .apply()
        val now = System.currentTimeMillis()
        return synchronized(logLock) {
            pruneExpiredLogsLocked(now)
            transientLogs.joinToString("\n") { it.line }
        }
    }

    fun clearLogs(context: Context) {
        synchronized(logLock) {
            transientLogs.clear()
        }
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .remove(KEY_LOGS)
            .putInt(KEY_PASS_COUNT, 0)
            .apply()
    }

    private fun pruneExpiredLogsLocked(nowMs: Long) {
        transientLogs.removeAll { nowMs - it.tsMs > LOG_RETENTION_MS }
    }
}
