package com.advisorii.pagerbridge

import android.Manifest
import android.bluetooth.BluetoothManager
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.widget.Button
import android.widget.EditText
import android.widget.Switch
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity

class MainActivity : AppCompatActivity() {

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
    }

    private fun renderConfig() {
        val config = BridgePreferences.loadConfig(this)
        findViewById<EditText>(R.id.edtDeviceName).setText(config.deviceName)
        findViewById<EditText>(R.id.edtDeviceAddress).setText(config.deviceAddress)
        findViewById<EditText>(R.id.edtServiceUuid).setText(config.serviceUuid)
        findViewById<EditText>(R.id.edtRxUuid).setText(config.rxUuid)
        findViewById<Switch>(R.id.switchOngoingNotification).isChecked = config.ongoingNotification
        renderState()
    }

    private fun saveSettingsFromUi() {
        val config = BridgePreferences.PagerConfig(
            deviceName = findViewById<EditText>(R.id.edtDeviceName).text.toString(),
            deviceAddress = findViewById<EditText>(R.id.edtDeviceAddress).text.toString(),
            serviceUuid = findViewById<EditText>(R.id.edtServiceUuid).text.toString(),
            rxUuid = findViewById<EditText>(R.id.edtRxUuid).text.toString(),
            ongoingNotification = findViewById<Switch>(R.id.switchOngoingNotification).isChecked
        )
        BridgePreferences.saveConfig(this, config)
        BridgeForegroundService.sync(this)
        updateStatusText("Settings saved")
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
        val running = BridgePreferences.loadConfig(this).ongoingNotification
        val status = if (listenerEnabled) {
            "Ready. Notification listener enabled. Background indicator: ${if (running) "ON" else "OFF"}."
        } else {
            "Action needed: Enable notification access for this app."
        }
        findViewById<TextView>(R.id.txtStatus).text = if (extra.isNullOrBlank()) status else "$status\n$extra"
        renderState()
    }
}
