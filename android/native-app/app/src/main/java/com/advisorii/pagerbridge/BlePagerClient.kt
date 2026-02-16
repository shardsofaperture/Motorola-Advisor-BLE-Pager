package com.advisorii.pagerbridge

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.core.content.ContextCompat
import java.util.concurrent.atomic.AtomicBoolean
import java.util.UUID

object BlePagerClient {
    data class CommandResult(
        val success: Boolean,
        val message: String,
        val statusResponse: String? = null
    )

    @Volatile
    private var activeGatt: BluetoothGatt? = null

    fun sendToPager(context: Context, payload: String): Boolean {
        return startTransaction(context, payload, expectStatusRead = false, onResult = null)
    }

    fun sendCommand(context: Context, command: String, onResult: (CommandResult) -> Unit) {
        val normalized = command.trim()
        if (normalized.isBlank()) {
            onResult(CommandResult(success = false, message = "Command is empty"))
            return
        }
        val payload = if (normalized.endsWith("\n")) normalized else "$normalized\n"
        val started = startTransaction(context, payload, expectStatusRead = true, onResult = onResult)
        if (!started) {
            onResult(CommandResult(success = false, message = "Failed to start BLE command transaction"))
        }
    }

    private fun startTransaction(
        context: Context,
        payload: String,
        expectStatusRead: Boolean,
        onResult: ((CommandResult) -> Unit)?
    ): Boolean {
        if (!hasBluetoothPermission(context)) {
            return false
        }

        val config = BridgePreferences.loadConfig(context)
        val serviceUuid = runCatching { UUID.fromString(config.serviceUuid) }.getOrNull() ?: return false
        val rxUuid = runCatching { UUID.fromString(config.rxUuid) }.getOrNull() ?: return false
        val statusUuid = UUID.fromString("1b0ee9b4-e833-5a9e-354c-7e2d4a6b2b7f")

        val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager ?: return false
        val adapter = manager.adapter ?: return false
        if (!adapter.isEnabled) {
            return false
        }

        val target = findTarget(adapter, config.deviceAddress, config.deviceName) ?: return false
        connectAndWrite(
            context = context,
            device = target,
            serviceUuid = serviceUuid,
            rxUuid = rxUuid,
            statusUuid = if (expectStatusRead) statusUuid else null,
            payload = payload,
            onResult = onResult
        )
        return true
    }

    private fun findTarget(adapter: BluetoothAdapter, address: String, name: String): BluetoothDevice? {
        if (address.isNotBlank()) {
            val direct = runCatching { adapter.getRemoteDevice(address) }.getOrNull()
            if (direct != null) return direct
        }
        val byAddress = adapter.bondedDevices.firstOrNull { it.address.equals(address, ignoreCase = true) }
        if (byAddress != null) return byAddress
        return adapter.bondedDevices.firstOrNull { it.name == name }
    }

    @SuppressLint("MissingPermission")
    private fun connectAndWrite(
        context: Context,
        device: BluetoothDevice,
        serviceUuid: UUID,
        rxUuid: UUID,
        statusUuid: UUID?,
        payload: String,
        onResult: ((CommandResult) -> Unit)?
    ) {
        activeGatt?.close()
        activeGatt = null
        val finished = AtomicBoolean(false)
        val mainHandler = Handler(Looper.getMainLooper())

        fun complete(result: CommandResult) {
            if (finished.compareAndSet(false, true)) {
                onResult?.let { callback ->
                    mainHandler.post { callback(result) }
                }
            }
        }

        val gatt = device.connectGatt(context, false, object : BluetoothGattCallback() {
            override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    complete(CommandResult(success = false, message = "Connect failed: $status"))
                    g.close()
                    if (activeGatt == g) activeGatt = null
                    return
                }
                if (newState == BluetoothProfile.STATE_CONNECTED) {
                    g.discoverServices()
                } else {
                    if (!finished.get()) {
                        complete(CommandResult(success = false, message = "Disconnected before completion"))
                    }
                    g.close()
                    if (activeGatt == g) activeGatt = null
                }
            }

            override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    complete(CommandResult(success = false, message = "Service discovery failed: $status"))
                    g.disconnect()
                    return
                }
                val service: BluetoothGattService = g.getService(serviceUuid) ?: run {
                    complete(CommandResult(success = false, message = "Service UUID not found"))
                    g.disconnect()
                    return
                }
                val characteristic: BluetoothGattCharacteristic = service.getCharacteristic(rxUuid) ?: run {
                    complete(CommandResult(success = false, message = "RX characteristic not found"))
                    g.disconnect()
                    return
                }

                val bytes = payload.toByteArray()
                characteristic.value = bytes
                characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    val queued = g.writeCharacteristic(
                        characteristic,
                        bytes,
                        BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                    )
                    if (queued != BluetoothGatt.GATT_SUCCESS) {
                        complete(CommandResult(success = false, message = "Write enqueue failed: $queued"))
                        g.disconnect()
                    }
                } else {
                    @Suppress("DEPRECATION")
                    if (!g.writeCharacteristic(characteristic)) {
                        complete(CommandResult(success = false, message = "Write enqueue failed"))
                        g.disconnect()
                    }
                }
            }

            override fun onCharacteristicWrite(
                g: BluetoothGatt,
                characteristic: BluetoothGattCharacteristic,
                status: Int
            ) {
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    complete(CommandResult(success = false, message = "Write failed: $status"))
                    g.disconnect()
                    return
                }

                if (statusUuid == null) {
                    complete(CommandResult(success = true, message = "Command written"))
                    g.disconnect()
                    return
                }

                val service = g.getService(serviceUuid)
                val statusCharacteristic = service?.getCharacteristic(statusUuid)
                if (statusCharacteristic == null) {
                    complete(CommandResult(success = false, message = "Status characteristic not found"))
                    g.disconnect()
                    return
                }

                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    val readRc = g.readCharacteristic(statusCharacteristic)
                    if (!readRc) {
                        complete(CommandResult(success = false, message = "Status read failed to start"))
                        g.disconnect()
                    }
                } else {
                    @Suppress("DEPRECATION")
                    if (!g.readCharacteristic(statusCharacteristic)) {
                        complete(CommandResult(success = false, message = "Status read failed to start"))
                        g.disconnect()
                    }
                }
            }

            override fun onCharacteristicRead(
                g: BluetoothGatt,
                characteristic: BluetoothGattCharacteristic,
                value: ByteArray,
                status: Int
            ) {
                handleStatusReadResult(g, status, value)
            }

            @Suppress("DEPRECATION")
            override fun onCharacteristicRead(
                g: BluetoothGatt,
                characteristic: BluetoothGattCharacteristic,
                status: Int
            ) {
                handleStatusReadResult(g, status, characteristic.value ?: ByteArray(0))
            }

            private fun handleStatusReadResult(
                g: BluetoothGatt,
                status: Int,
                value: ByteArray
            ) {
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    complete(CommandResult(success = false, message = "Status read failed: $status"))
                    g.disconnect()
                    return
                }
                val response = value.toString(Charsets.UTF_8).ifBlank { "<empty>" }
                complete(CommandResult(success = true, message = "Command written + status read", statusResponse = response))
                g.disconnect()
            }
        })
        activeGatt = gatt
    }

    private fun hasBluetoothPermission(context: Context): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            return true
        }
        return ContextCompat.checkSelfPermission(
            context,
            Manifest.permission.BLUETOOTH_CONNECT
        ) == PackageManager.PERMISSION_GRANTED
    }
}
