#include <Arduino.h>
#include <NimBLEDevice.h>

namespace {
constexpr const char *kDeviceName = "PagerBridge";

static NimBLEServer *bleServer = nullptr;
static NimBLECharacteristic *rxCharacteristic = nullptr;
static NimBLECharacteristic *statusCharacteristic = nullptr;

static const NimBLEUUID kServiceUUID("1b0ee9b4-e833-5a9e-354c-7e2d486b2b7f");
static const NimBLEUUID kRxUUID("1b0ee9b4-e833-5a9e-354c-7e2d496b2b7f");
static const NimBLEUUID kStatusUUID("1b0ee9b4-e833-5a9e-354c-7e2d4a6b2b7f");

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) override {
    Serial.printf("BLE connected: %s\n", connInfo.getAddress().toString().c_str());
  }

  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override {
    Serial.printf("BLE disconnected: %s reason=%d\n", connInfo.getAddress().toString().c_str(), reason);
    NimBLEDevice::startAdvertising();
  }
};

class RxCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &connInfo) override {
    std::string value = characteristic->getValue();
    Serial.printf("RX from %s: %s\n", connInfo.getAddress().toString().c_str(), value.c_str());

    if (statusCharacteristic != nullptr) {
      statusCharacteristic->setValue("OK");
      statusCharacteristic->notify();
    }
  }
};
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("Starting BLE PagerBridge (Arduino/NimBLE)...");

  NimBLEDevice::init(kDeviceName);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  NimBLEService *service = bleServer->createService(kServiceUUID);
  rxCharacteristic = service->createCharacteristic(
      kRxUUID, NIMBLE_PROPERTY::WRITE);
  rxCharacteristic->setCallbacks(new RxCallbacks());

  statusCharacteristic = service->createCharacteristic(
      kStatusUUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  statusCharacteristic->setValue("READY");

  service->start();

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUUID);
  advertising->setScanResponse(true);
  advertising->start();

  Serial.println("BLE advertising started.");
}

void loop() {
  static uint32_t lastLogMs = 0;
  const uint32_t now = millis();
  if (now - lastLogMs >= 5000) {
    lastLogMs = now;
    Serial.println("Heartbeat: waiting for BLE writes...");
  }
  delay(10);
}
