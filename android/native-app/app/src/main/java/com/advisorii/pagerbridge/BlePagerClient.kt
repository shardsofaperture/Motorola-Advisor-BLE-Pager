package com.advisorii.pagerbridge

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat
import java.util.UUID

object BlePagerClient {
    private val serviceUuid: UUID = UUID.fromString("1b0ee9b4-e833-5a9e-354c-7e2d486b2b7f")
    private val rxUuid: UUID = UUID.fromString("1b0ee9b4-e833-5a9e-354c-7e2d496b2b7f")
    private const val pagerDeviceName = "PagerBridge"

    fun sendToPager(context: Context, payload: String) {
        if (!hasBluetoothPermission(context)) {
            return
        }

        val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager ?: return
        val adapter = manager.adapter ?: return
        if (!adapter.isEnabled) {
            return
        }

        val target = findConnectedTarget(adapter)
            ?: findBondedTarget(adapter)
            ?: return

        connectAndWrite(context, target, payload)
    }

    private fun findConnectedTarget(adapter: BluetoothAdapter): BluetoothDevice? {
        return adapter.bondedDevices.firstOrNull { it.name == pagerDeviceName }
    }

    private fun findBondedTarget(adapter: BluetoothAdapter): BluetoothDevice? {
        return adapter.bondedDevices.firstOrNull { it.name == pagerDeviceName }
    }

    @SuppressLint("MissingPermission")
    private fun connectAndWrite(context: Context, device: BluetoothDevice, payload: String) {
        var gatt: BluetoothGatt? = null
        gatt = device.connectGatt(context, false, object : BluetoothGattCallback() {
            override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
                if (newState == BluetoothProfile.STATE_CONNECTED) {
                    g.discoverServices()
                } else {
                    g.close()
                }
            }

            override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
                val service: BluetoothGattService = g.getService(serviceUuid) ?: run {
                    g.disconnect()
                    return
                }
                val characteristic: BluetoothGattCharacteristic =
                    service.getCharacteristic(rxUuid) ?: run {
                        g.disconnect()
                        return
                    }

                characteristic.value = payload.toByteArray()
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    g.writeCharacteristic(characteristic, payload.toByteArray(), BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
                } else {
                    @Suppress("DEPRECATION")
                    g.writeCharacteristic(characteristic)
                }
            }

            override fun onCharacteristicWrite(
                g: BluetoothGatt,
                characteristic: BluetoothGattCharacteristic,
                status: Int
            ) {
                g.disconnect()
            }

            override fun onDescriptorWrite(
                gatt: BluetoothGatt,
                descriptor: BluetoothGattDescriptor,
                status: Int
            ) {
                // No-op
            }
        })
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
