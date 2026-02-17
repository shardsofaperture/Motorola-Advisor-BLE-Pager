package com.advisorii.pagerbridge

import android.Manifest
import android.bluetooth.BluetoothManager
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.widget.Button
import android.widget.EditText
import android.widget.Switch
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity

class MainActivity : AppCompatActivity() {
    private val uiHandler = Handler(Looper.getMainLooper())
    private val supportedTxPowerDbm = listOf(-24, -21, -18, -15, -12, -9, -6, -3, 0, 3, 6, 9, 12, 15, 18, 20)
    private val supportedTxPowerDbmSet = supportedTxPowerDbm.toSet()
    private val logRefreshRunnable = object : Runnable {
        override fun run() {
            renderState()
            uiHandler.postDelayed(this, 1000L)
        }
    }

    private val requestBluetoothPermissions = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { updateStatusText() }

    private val requestNotificationPermission = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { updateStatusText() }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        findViewById<Button>(R.id.btnNotificationAccess).setOnClickListener {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        }

        findViewById<Button>(R.id.btnBluetoothPermissions).setOnClickListener {
            val permissions = mutableListOf<String>()
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                permissions += Manifest.permission.BLUETOOTH_SCAN
                permissions += Manifest.permission.BLUETOOTH_CONNECT
            }
            if (permissions.isNotEmpty()) {
                requestBluetoothPermissions.launch(permissions.toTypedArray())
            }
        }

        findViewById<Button>(R.id.btnNotificationPermission).setOnClickListener {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                requestNotificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
            }
        }

        findViewById<Button>(R.id.btnSaveSettings).setOnClickListener {
            saveSettingsFromUi()
        }

        findViewById<Button>(R.id.btnTxPowerMinus6).setOnClickListener {
            applyTxPowerAndSend(-6)
        }

        findViewById<Button>(R.id.btnTxPowerMinus12).setOnClickListener {
            applyTxPowerAndSend(-12)
        }

        findViewById<Button>(R.id.btnTxPower0).setOnClickListener {
            applyTxPowerAndSend(0)
        }

        findViewById<Button>(R.id.btnTxPowerMinus3).setOnClickListener {
            applyTxPowerAndSend(-3)
        }

        findViewById<Button>(R.id.btnApplyCustomTxPower).setOnClickListener {
            applyCustomTxPowerAndSend()
        }

        findViewById<Button>(R.id.btnSendDirectCommand).setOnClickListener {
            val command = findViewById<EditText>(R.id.edtDirectCommand).text.toString()
            sendDirectCommand(command)
        }

        findViewById<Button>(R.id.btnQueryMetrics).setOnClickListener {
            sendDirectCommand("metrics")
        }

        findViewById<Button>(R.id.btnQueryStatus).setOnClickListener {
            sendDirectCommand("status")
        }

        findViewById<Button>(R.id.btnQueryDiagnostics).setOnClickListener {
            sendDirectCommand("report")
        }

        findViewById<Button>(R.id.btnStopBridge).setOnClickListener {
            val current = BridgePreferences.loadConfig(this)
            BridgePreferences.saveConfig(
                this,
                current.copy(
                    forwardingEnabled = false,
                    ongoingNotification = false
                )
            )
            BridgeForegroundService.sync(this)
            updateStatusText("Bridge stopped")
            finishAffinity()
        }

        findViewById<Button>(R.id.btnSelectDevice).setOnClickListener {
            showBondedDevicesPicker()
        }

        findViewById<Button>(R.id.btnClearLog).setOnClickListener {
            BridgePreferences.clearLogs(this)
            renderState()
            BridgeForegroundService.sync(this)
        }

        renderConfig()
        BridgeForegroundService.sync(this)
        updateStatusText()
    }

    override fun onResume() {
        super.onResume()
        updateStatusText()
        uiHandler.removeCallbacks(logRefreshRunnable)
        uiHandler.post(logRefreshRunnable)
    }

    override fun onPause() {
        super.onPause()
        uiHandler.removeCallbacks(logRefreshRunnable)
    }

    private fun renderConfig() {
        val config = BridgePreferences.loadConfig(this)
        findViewById<EditText>(R.id.edtDeviceName).setText(config.deviceName)
        findViewById<EditText>(R.id.edtDeviceAddress).setText(config.deviceAddress)
        findViewById<EditText>(R.id.edtServiceUuid).setText(config.serviceUuid)
        findViewById<EditText>(R.id.edtRxUuid).setText(config.rxUuid)
        findViewById<EditText>(R.id.edtCustomTxPower).setText(BridgePreferences.getCustomTxPowerText(this))
        findViewById<Switch>(R.id.switchOngoingNotification).isChecked = config.ongoingNotification
        findViewById<Switch>(R.id.switchForwardingEnabled).isChecked = config.forwardingEnabled
        renderTxPowerState()
        renderState()
    }

    private fun saveSettingsFromUi() {
        val config = BridgePreferences.PagerConfig(
            deviceName = findViewById<EditText>(R.id.edtDeviceName).text.toString(),
            deviceAddress = findViewById<EditText>(R.id.edtDeviceAddress).text.toString(),
            serviceUuid = findViewById<EditText>(R.id.edtServiceUuid).text.toString(),
            rxUuid = findViewById<EditText>(R.id.edtRxUuid).text.toString(),
            ongoingNotification = findViewById<Switch>(R.id.switchOngoingNotification).isChecked,
            forwardingEnabled = findViewById<Switch>(R.id.switchForwardingEnabled).isChecked
        )
        BridgePreferences.saveConfig(this, config)
        BridgeForegroundService.sync(this)
        updateStatusText("Settings saved")
    }

    private fun sendDirectCommand(command: String) {
        BlePagerClient.sendCommand(this, command) { result ->
            val statusLine = if (result.success) {
                "Command ok: ${result.message}"
            } else {
                "Command failed: ${result.message}"
            }
            val responseLine = result.statusResponse?.let { "Status char: $it" } ?: ""
            val extra = if (responseLine.isBlank()) statusLine else "$statusLine\n$responseLine"
            BridgePreferences.appendLog(this, extra)
            updateStatusText(extra)
        }
    }

    private fun applyCustomTxPowerAndSend() {
        val raw = findViewById<EditText>(R.id.edtCustomTxPower).text.toString().trim()
        if (raw.isBlank()) {
            updateStatusText("Custom TX power is empty")
            return
        }
        val dbm = raw.toIntOrNull()
        if (dbm == null) {
            updateStatusText("Custom TX power must be a whole number")
            return
        }
        if (!supportedTxPowerDbmSet.contains(dbm)) {
            updateStatusText("Unsupported TX power $dbm dBm. Use: ${supportedTxPowerDbm.joinToString(", ")}")
            return
        }
        BridgePreferences.setCustomTxPowerText(this, raw)
        applyTxPowerAndSend(dbm)
    }

    private fun applyTxPowerAndSend(dbm: Int) {
        if (!supportedTxPowerDbmSet.contains(dbm)) {
            updateStatusText("Unsupported TX power $dbm dBm")
            return
        }
        BridgePreferences.setLastTxPowerDbm(this, dbm)
        BridgePreferences.setCustomTxPowerText(this, dbm.toString())
        findViewById<EditText>(R.id.edtCustomTxPower).setText(dbm.toString())
        renderTxPowerState()
        sendDirectCommand("txpower $dbm")
    }

    private fun renderTxPowerState() {
        val last = BridgePreferences.getLastTxPowerDbm(this)
        findViewById<TextView>(R.id.txtTxPowerState).text = "Saved TX power: $last dBm"
    }

    private fun renderState() {
        findViewById<TextView>(R.id.txtPassCount).text = "Messages passed: ${BridgePreferences.getPassCount(this)}"
        val logs = BridgePreferences.getLogs(this)
        findViewById<TextView>(R.id.txtLog).text = if (logs.isBlank()) "No messages yet." else logs
    }

    private fun showBondedDevicesPicker() {
        val manager = getSystemService(BLUETOOTH_SERVICE) as? BluetoothManager ?: return
        val devices = manager.adapter?.bondedDevices?.toList().orEmpty()
        if (devices.isEmpty()) {
            updateStatusText("No bonded BLE devices found")
            return
        }

        val labels = devices.map { "${it.name ?: "Unknown"}\n${it.address}" }.toTypedArray()
        AlertDialog.Builder(this)
            .setTitle("Select paired BLE device")
            .setItems(labels) { _, which ->
                val selected = devices[which]
                findViewById<EditText>(R.id.edtDeviceName).setText(selected.name ?: "")
                findViewById<EditText>(R.id.edtDeviceAddress).setText(selected.address)
            }
            .show()
    }

    private fun updateStatusText(extra: String? = null) {
        val listenerEnabled = NotificationAccessState.isNotificationListenerEnabled(this)
        val config = BridgePreferences.loadConfig(this)
        val running = config.ongoingNotification
        val forwarding = config.forwardingEnabled
        val status = if (listenerEnabled) {
            "Ready. Listener enabled. Forwarding: ${if (forwarding) "ON" else "OFF"}. Background indicator: ${if (running) "ON" else "OFF"}."
        } else {
            "Action needed: Enable notification access for this app."
        }
        findViewById<TextView>(R.id.txtStatus).text = if (extra.isNullOrBlank()) status else "$status\n$extra"
        renderState()
    }
}
