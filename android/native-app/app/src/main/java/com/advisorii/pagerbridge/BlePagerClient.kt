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
import androidx.core.content.ContextCompat
import java.util.UUID

object BlePagerClient {
    @Volatile
    private var activeGatt: BluetoothGatt? = null

    fun sendToPager(context: Context, payload: String): Boolean {
        if (!hasBluetoothPermission(context)) {
            return false
        }

        val config = BridgePreferences.loadConfig(context)
        val serviceUuid = runCatching { UUID.fromString(config.serviceUuid) }.getOrNull() ?: return false
        val rxUuid = runCatching { UUID.fromString(config.rxUuid) }.getOrNull() ?: return false

        val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager ?: return false
        val adapter = manager.adapter ?: return false
        if (!adapter.isEnabled) {
            return false
        }

        val target = findTarget(adapter, config.deviceAddress, config.deviceName) ?: return false
        connectAndWrite(context, target, serviceUuid, rxUuid, payload)
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
        payload: String
    ) {
        activeGatt?.close()
        activeGatt = null

        val gatt = device.connectGatt(context, false, object : BluetoothGattCallback() {
            override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    g.close()
                    if (activeGatt == g) activeGatt = null
                    return
                }
                if (newState == BluetoothProfile.STATE_CONNECTED) {
                    g.discoverServices()
                } else {
                    g.close()
                    if (activeGatt == g) activeGatt = null
                }
            }

            override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    g.disconnect()
                    return
                }
                val service: BluetoothGattService = g.getService(serviceUuid) ?: run {
                    g.disconnect()
                    return
                }
                val characteristic: BluetoothGattCharacteristic = service.getCharacteristic(rxUuid) ?: run {
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
                        g.disconnect()
                    }
                } else {
                    @Suppress("DEPRECATION")
                    if (!g.writeCharacteristic(characteristic)) {
                        g.disconnect()
                    }
                }
            }

            override fun onCharacteristicWrite(
                g: BluetoothGatt,
                characteristic: BluetoothGattCharacteristic,
                status: Int
            ) {
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
