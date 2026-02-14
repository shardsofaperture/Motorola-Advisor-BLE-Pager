#include <Arduino.h>
#include <SPIFFS.h>
#include <NimBLEDevice.h>

#include <vector>

#include "driver/gpio.h"
#include "driver/rmt.h"
#if __has_include("esp_pm.h")
#include "esp_pm.h"
#define HAS_ESP_PM 1
#else
#define HAS_ESP_PM 0
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {
constexpr const char *kConfigPath = "/config.json";
constexpr uint32_t kSyncWord = 0x7CD215D8;
constexpr uint32_t kIdleWord = 0x7A89C197;
constexpr uint32_t kMaxRmtDuration = 32767;
constexpr size_t kMaxRmtItems = 10000;
constexpr size_t kMaxMessageBatches = 10;
constexpr size_t kMaxBleLineLength = 512;
constexpr uint32_t kCommandWarmupWindowMs = 150;
constexpr uint32_t kCommandWarmupDelayMs = 250;
constexpr int kUserLedPin = 21;
constexpr TickType_t kBootLedOnTicks = pdMS_TO_TICKS(5000);
constexpr TickType_t kHeartbeatPeriodTicks = pdMS_TO_TICKS(15000);
constexpr TickType_t kHeartbeatPulseTicks = pdMS_TO_TICKS(15);
constexpr const char *kBleServiceUuid = "1b0ee9b4-e833-5a9e-354c-7e2d486b2b7f";
constexpr const char *kBleRxUuid = "1b0ee9b4-e833-5a9e-354c-7e2d496b2b7f";
constexpr const char *kBleStatusUuid = "1b0ee9b4-e833-5a9e-354c-7e2d4a6b2b7f";
}

enum class OutputMode : uint8_t { kOpenDrain = 0, kPushPull = 1 };

struct Config {
  uint32_t baud = 512;
  uint32_t preambleBits = 576;
  uint32_t capInd = 1422890;
  uint32_t capGrp = 1422890;
  uint8_t functionBits = 2;
  int dataGpio = 4;
  OutputMode output = OutputMode::kPushPull;
  bool invertWords = false;
  bool driveOneLow = true;
  bool idleHigh = true;
  bool blockingTx = false;
  String bootPreset = "ADVISOR";
  bool firstBootShown = false;
};

static Config config;

static void printStatus();
static void printPmStatus();
static void handleCommand(const String &line);
static void notifyStatus(const char *status, bool alwaysNotify);
static void printErr(const char *tag, esp_err_t err) {
  if (err == ESP_OK) {
    return;
  }
  Serial.printf("ERR %s: 0x%X\n", tag, static_cast<unsigned int>(err));
}

struct TxJob {
  std::vector<uint8_t> bits;
  uint32_t baud;
  int gpio;
  OutputMode output;
  bool idleHigh;
  bool driveOneLow;
};

static QueueHandle_t txQueue = nullptr;
static QueueHandle_t commandQueue = nullptr;
static bool gPmCompiled = false;
static bool gPmConfigured = false;
static esp_err_t gPmConfigureErr = ESP_FAIL;
static uint32_t gPmMaxFreq = 80;
static uint32_t gPmMinFreq = 20;
static bool gPmLightSleepRequested = true;
// Debug default: keep BLE bring-up stable by avoiding automatic light sleep.
static bool gBleDebugNoLightSleep = true;
static volatile bool gTxNoLightSleepLockHeld = false;

class WaveTx {
 public:
  bool isBusy() const { return busy_; }

  bool transmitBits(const std::vector<uint8_t> &bits, uint32_t baud, int gpio,
                    OutputMode output, bool idleHigh, bool driveOneLow, bool blockingTx) {
    if (busy_) {
      return false;
    }
    auto stopAndSetIdle = [&]() {
      if (initialized_) {
        rmt_tx_stop(channel_);
      }
      setIdle(gpio, output, idleHigh);
    };
    if (bits.empty()) {
      stopAndSetIdle();
      return true;
    }
    uint32_t bitPeriodUs = (1000000 + (baud / 2)) / baud;
    buildItems(bits, bitPeriodUs, driveOneLow);
    if (overflowed_) {
      Serial.println("ERR: RMT_ITEMS_OVERFLOW");
      notifyStatus("TX_TOO_LONG", true);
      stopAndSetIdle();
      return false;
    }
    if (items_.empty()) {
      stopAndSetIdle();
      return true;
    }
    if (!ensureConfig(gpio, output, idleHigh)) {
      stopAndSetIdle();
      return false;
    }

    auto cleanupTxState = [&]() {
      releasePmLock();
      busy_ = false;
      if (initialized_) {
        rmt_tx_stop(channel_);
      }
      setIdle(gpio, output, idleHigh);
    };
    struct CleanupGuard {
      decltype(cleanupTxState) &fn;
      bool active;
      CleanupGuard(decltype(cleanupTxState) &cleanupFn, bool isActive)
          : fn(cleanupFn), active(isActive) {}
      ~CleanupGuard() {
        if (active) {
          fn();
        }
      }
    } cleanupGuard{cleanupTxState, false};

    busy_ = true;
    acquirePmLock();
    cleanupGuard.active = true;
    setLineDrive(gpio, output, idleHigh);

    bool ok = true;
    esp_err_t err = rmt_write_items(channel_, items_.data(), items_.size(), false);
    printErr("rmt_write_items", err);
    if (err != ESP_OK) {
      ok = false;
    }

    if (ok) {
      const TickType_t timeoutTicks = computeTimeoutTicks(bits.size(), baud, blockingTx);
      err = rmt_wait_tx_done(channel_, timeoutTicks);
      if (err == ESP_ERR_TIMEOUT) {
        Serial.println("ERR: RMT_TX_TIMEOUT");
        ok = false;
      } else if (err != ESP_OK) {
        printErr("rmt_wait_tx_done", err);
        ok = false;
      }
    }

    if (!ok) {
      recoverFromTxFailure(gpio, output, idleHigh);
    }
    return ok;
  }

 private:
  static TickType_t computeTimeoutTicks(size_t bitCount, uint32_t baud, bool blockingTx) {
    if (baud == 0) {
      return pdMS_TO_TICKS(6000);
    }
    uint64_t txMs = (static_cast<uint64_t>(bitCount) * 1000ULL) / static_cast<uint64_t>(baud);
    txMs += blockingTx ? 250ULL : 50ULL;
    if (txMs > 6000ULL) {
      txMs = 6000ULL;
    }
    if (txMs == 0ULL) {
      txMs = 1ULL;
    }
    return pdMS_TO_TICKS(static_cast<uint32_t>(txMs));
  }

  void acquirePmLock() {
#if HAS_ESP_PM && defined(CONFIG_PM_ENABLE)
    if (!pmLockInitialized_) {
      esp_err_t err =
          esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "rmt_tx", &pmNoLightSleepLock_);
      if (err != ESP_OK) {
        pmNoLightSleepLock_ = nullptr;
      }
      pmLockInitialized_ = true;
    }
    if (pmNoLightSleepLock_) {
      esp_err_t err = esp_pm_lock_acquire(pmNoLightSleepLock_);
      if (err != ESP_OK) {
        printErr("esp_pm_lock_acquire", err);
      } else {
        gTxNoLightSleepLockHeld = true;
      }
    } else {
      gTxNoLightSleepLockHeld = false;
    }
#endif
  }

  void releasePmLock() {
#if HAS_ESP_PM && defined(CONFIG_PM_ENABLE)
    if (pmNoLightSleepLock_) {
      esp_err_t err = esp_pm_lock_release(pmNoLightSleepLock_);
      if (err != ESP_OK) {
        printErr("esp_pm_lock_release", err);
      } else {
        gTxNoLightSleepLockHeld = false;
      }
    } else {
      gTxNoLightSleepLockHeld = false;
    }
#else
    gTxNoLightSleepLockHeld = false;
#endif
  }

  bool ensureConfig(int gpio, OutputMode output, bool idleHigh) {
    if (initialized_ && gpio == gpio_ && output == output_ && idleHigh == idleHigh_) {
      return true;
    }
    if (initialized_) {
      rmt_driver_uninstall(channel_);
      initialized_ = false;
    }
    gpio_ = gpio;
    output_ = output;
    idleHigh_ = idleHigh;
    rmt_config_t config = {};
    config.rmt_mode = RMT_MODE_TX;
    config.channel = channel_;
    config.gpio_num = static_cast<gpio_num_t>(gpio_);
    config.mem_block_num = 4;
    config.clk_div = 80;
    config.tx_config.loop_en = false;
    config.tx_config.carrier_en = false;
    config.tx_config.idle_output_en = false;
    config.tx_config.idle_level = idleHigh_ ? RMT_IDLE_LEVEL_HIGH : RMT_IDLE_LEVEL_LOW;
    esp_err_t err = rmt_config(&config);
    printErr("rmt_config", err);
    if (err != ESP_OK) {
      return false;
    }
    err = rmt_driver_install(channel_, 0, 0);
    printErr("rmt_driver_install", err);
    if (err != ESP_OK) {
      return false;
    }
    initialized_ = true;
    return true;
  }

  void recoverFromTxFailure(int gpio, OutputMode output, bool idleHigh) {
    if (initialized_) {
      rmt_tx_stop(channel_);
    }
    if (initialized_) {
      rmt_driver_uninstall(channel_);
      initialized_ = false;
    }
    if (!ensureConfig(gpio, output, idleHigh)) {
      Serial.println("ERR: RMT_RECOVER");
    }
  }

  void buildItems(const std::vector<uint8_t> &bits, uint32_t bitPeriodUs, bool driveOneLow) {
    items_.clear();
    overflowed_ = false;
    if (bits.empty()) {
      return;
    }
    size_t index = 0;
    while (index < bits.size()) {
      uint8_t value = bits[index];
      size_t runLength = 1;
      while ((index + runLength) < bits.size() && bits[index + runLength] == value) {
        ++runLength;
      }
      uint32_t totalDuration = static_cast<uint32_t>(runLength) * bitPeriodUs;
      bool levelHigh = driveOneLow ? (value == 0) : (value != 0);
      while (totalDuration > 0) {
        uint32_t chunk = totalDuration > kMaxRmtDuration ? kMaxRmtDuration : totalDuration;
        rmt_item32_t item = {};
        if (chunk > 1) {
          item.duration0 = chunk - 1;
          item.level0 = levelHigh;
          item.duration1 = 1;
          item.level1 = levelHigh;
        } else {
          item.duration0 = 1;
          item.level0 = levelHigh;
          item.duration1 = 1;
          item.level1 = levelHigh;
        }
        items_.push_back(item);
        if (items_.size() > kMaxRmtItems) {
          overflowed_ = true;
          items_.clear();
          return;
        }
        totalDuration -= chunk;
      }
      index += runLength;
    }
  }

  void setLineDrive(int gpio, OutputMode output, bool idleHigh) {
    if (output == OutputMode::kOpenDrain) {
      pinMode(gpio, OUTPUT_OPEN_DRAIN);
    } else {
      pinMode(gpio, OUTPUT);
    }
    digitalWrite(gpio, idleHigh ? HIGH : LOW);
  }

  void setIdle(int gpio, OutputMode output, bool idleHigh) {
    if (output == OutputMode::kOpenDrain && idleHigh) {
      pinMode(gpio, OUTPUT_OPEN_DRAIN);
      digitalWrite(gpio, HIGH);
      return;
    }
    setLineHiZ(gpio);
  }

  void setLineHiZ(int gpio) {
    pinMode(gpio, INPUT);
    gpio_pullup_dis(static_cast<gpio_num_t>(gpio));
    gpio_pulldown_dis(static_cast<gpio_num_t>(gpio));
  }

  rmt_channel_t channel_ = RMT_CHANNEL_0;
  bool initialized_ = false;
  bool busy_ = false;
  bool overflowed_ = false;
  int gpio_ = -1;
  OutputMode output_ = OutputMode::kOpenDrain;
  bool idleHigh_ = true;
#if HAS_ESP_PM && defined(CONFIG_PM_ENABLE)
  esp_pm_lock_handle_t pmNoLightSleepLock_ = nullptr;
  bool pmLockInitialized_ = false;
#endif
  std::vector<rmt_item32_t> items_;
};

class PocsagEncoder {
 public:
  std::vector<std::vector<uint32_t>> buildBatchWords(uint32_t capcode, uint8_t functionBits,
                                                     const String &message) const {
    return buildAlphaBatches(capcode, functionBits, message);
  }

  uint32_t buildAddressCodeword(uint32_t capcode, uint8_t functionBits) const {
    return buildAddressWord(capcode, functionBits);
  }

 private:
  std::vector<std::vector<uint32_t>> buildAlphaBatches(uint32_t capcode, uint8_t functionBits,
                                                       const String &message) const {
    std::vector<std::vector<uint32_t>> batches;
    uint8_t frame = static_cast<uint8_t>(capcode & 0x7);
    uint32_t addressWord = buildAddressWord(capcode, functionBits);
    std::vector<uint32_t> messageWords = buildAlphaWords(message);

    std::vector<uint32_t> firstBatch(16, kIdleWord);
    size_t index = static_cast<size_t>(frame) * 2;
    if (index < firstBatch.size()) {
      firstBatch[index++] = addressWord;
    }

    size_t msgIndex = 0;
    while (index < firstBatch.size() && msgIndex < messageWords.size()) {
      firstBatch[index++] = messageWords[msgIndex++];
    }
    batches.push_back(firstBatch);

    while (msgIndex < messageWords.size()) {
      if (batches.size() >= kMaxMessageBatches) {
        Serial.printf("WARN: message truncated to %u batches\n",
                      static_cast<unsigned int>(kMaxMessageBatches));
        break;
      }
      std::vector<uint32_t> continuationBatch(16, kIdleWord);
      size_t continuationIndex = 0;
      while (continuationIndex < continuationBatch.size() && msgIndex < messageWords.size()) {
        continuationBatch[continuationIndex++] = messageWords[msgIndex++];
      }
      batches.push_back(continuationBatch);
    }

    return batches;
  }

  std::vector<uint32_t> buildAlphaWords(const String &message) const {
    std::vector<uint8_t> bits;
    bits.reserve(message.length() * 7);
    for (size_t i = 0; i < message.length(); ++i) {
      uint8_t value = static_cast<uint8_t>(message[i]) & 0x7F;
      for (int b = 0; b < 7; ++b) {
        bits.push_back(static_cast<uint8_t>((value >> b) & 0x1));
      }
    }
    std::vector<uint32_t> words;
    size_t index = 0;
    while (index < bits.size()) {
      uint32_t data = 0;
      for (int i = 0; i < 20; ++i) {
        data <<= 1;
        if (index < bits.size()) {
          data |= bits[index++];
        }
      }
      words.push_back(buildMessageWord(data));
    }
    if (words.empty()) {
      words.push_back(buildMessageWord(0));
    }
    return words;
  }

  uint32_t buildAddressWord(uint32_t capcode, uint8_t functionBits) const {
    uint32_t address = capcode >> 3;
    uint32_t data = (address & 0x3FFFF) << 2;
    data |= (functionBits & 0x3);
    uint32_t msg21 = data & 0x1FFFFF;
    return encodeCodeword(msg21);
  }

  uint32_t buildMessageWord(uint32_t data20) const {
    uint32_t msg21 = (1u << 20) | (data20 & 0xFFFFF);
    return encodeCodeword(msg21);
  }

  uint32_t crc(uint32_t msg21) const {
    uint32_t reg = msg21 << 10;
    constexpr uint32_t poly = 0x769;
    for (int i = 30; i >= 10; --i) {
      if (reg & (1u << i)) {
        reg ^= (poly << (i - 10));
      }
    }
    return reg & 0x3FF;
  }

  uint32_t parity(uint32_t value) const {
    uint32_t bit = 0;
    while (value) {
      bit ^= (value & 1u);
      value >>= 1;
    }
    return bit & 0x1;
  }

  uint32_t encodeCodeword(uint32_t msg21) const {
    uint32_t remainder = crc(msg21);
    uint32_t word = (msg21 << 11) | (remainder << 1);
    uint32_t parityBit = parity(word);
    return word | parityBit;
  }
};

static PocsagEncoder encoder;
static WaveTx waveTx;
static volatile bool gWorkerBusy = false;
static bool gBleConnected = false;
static uint32_t gBleConnectedAtMs = 0;
static NimBLECharacteristic *gRxChar = nullptr;
static NimBLECharacteristic *gStatusChar = nullptr;
static std::string gLastBlePayload;
static uint32_t gLastBlePayloadMs = 0;
constexpr uint32_t kBleDedupeWindowMs = 1500;
static String gLastSendLine;
static uint32_t gLastSendLineMs = 0;
constexpr uint32_t kSendDedupeWindowMs = 2000;

struct CommandJob {
  String line;
  uint32_t notBeforeMs;
};

static String gLastStatus;

static void notifyStatus(const char *status, bool alwaysNotify) {
  if (!status || !gStatusChar) {
    return;
  }
  String normalized = String(status);
  normalized.trim();
  if (!alwaysNotify && normalized == gLastStatus) {
    return;
  }
  gLastStatus = normalized;
  String payload = normalized + "\n";
  gStatusChar->setValue(payload.c_str());
  if (gBleConnected) {
    gStatusChar->notify();
  }
}

static bool enqueueCommand(const String &line, uint32_t notBeforeMs = 0) {
  if (!commandQueue) {
    Serial.println("ERR: CMD_QUEUE");
    notifyStatus("ERR:CMD_QUEUE", true);
    return false;
  }
  CommandJob *job = new CommandJob{line, notBeforeMs};
  if (xQueueSend(commandQueue, &job, 0) != pdTRUE) {
    delete job;
    Serial.println("ERR: CMD_QUEUE_FULL");
    notifyStatus("ERR:CMD_QUEUE_FULL", true);
    return false;
  }
  return true;
}

class CommandParser {
 public:
  void handleInput(const std::string &input, uint32_t notBeforeMs = 0) {
    for (unsigned char c : input) {
      if (c == '\n' || c == '\r') {
        if (!overflowed_ && lineBuf_.length() > 0) {
          enqueueCommand(lineBuf_, notBeforeMs);
          lineBuf_ = "";
        } else if (overflowed_) {
          notifyStatus("ERR:LINE_TOO_LONG", true);
          lineBuf_ = "";
          overflowed_ = false;
        }
        continue;
      }
      if (overflowed_) {
        continue;
      }
      if (lineBuf_.length() >= kMaxBleLineLength) {
        overflowed_ = true;
        Serial.println("ERR: BLE command line too long");
        continue;
      }
      lineBuf_ += static_cast<char>(c);
    }
  }

 private:
  String lineBuf_;
  bool overflowed_ = false;
};

struct BleContext {
  CommandParser *parser;
  explicit BleContext(CommandParser *p = nullptr) : parser(p) {}
};

static CommandParser gBleParser;
static BleContext bleContext(&gBleParser);

class RxCallbacks : public NimBLECharacteristicCallbacks {
 public:
  void onWrite(NimBLECharacteristic *characteristic) override {
    std::string value = characteristic->getValue();
    uint32_t now = millis();
    if (value == gLastBlePayload && (now - gLastBlePayloadMs) < kBleDedupeWindowMs) {
      Serial.println("DROP_DUP_BLE");
      return;
    }
    gLastBlePayload = value;
    gLastBlePayloadMs = now;
    std::string input = value;
    if (input.empty() || (input.back() != '\n' && input.back() != '\r')) {
      input.push_back('\n');
    }

    String preview;
    constexpr size_t kMaxPreview = 80;
    preview.reserve(kMaxPreview);
    for (unsigned char c : value) {
      if (preview.length() >= kMaxPreview) {
        break;
      }
      if (c >= 32 && c <= 126) {
        preview += static_cast<char>(c);
      } else {
        preview += '.';
      }
    }
    Serial.printf("BLE RX %u bytes: %s\n", static_cast<unsigned int>(value.size()),
                  preview.c_str());

    if (bleContext.parser) {
      uint32_t delayUntilMs = 0;
      uint32_t elapsedSinceConnect = now - gBleConnectedAtMs;
      if (gBleConnected && elapsedSinceConnect < kCommandWarmupWindowMs) {
        delayUntilMs = gBleConnectedAtMs + kCommandWarmupDelayMs;
      }
      bleContext.parser->handleInput(input, delayUntilMs);
    }
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
 public:
  void onConnect(NimBLEServer *server, ble_gap_conn_desc *desc) override {
    gBleConnected = true;
    gBleConnectedAtMs = millis();
    Serial.println("BLE: connected");
    notifyStatus("CONNECTED", true);
    // NimBLEDevice::getAdvertising()->stop();
    server->updateConnParams(desc->conn_handle, 24, 48, 0, 400);
  }

  void onDisconnect(NimBLEServer *server) override {
    (void)server;
    gBleConnected = false;
    Serial.println("BLE: disconnected");
    notifyStatus("READY", true);
    NimBLEDevice::getAdvertising()->start();
  }
};

static void setupPowerManagement() {
  if (gBleDebugNoLightSleep) {
    gPmMaxFreq = 80;
    gPmMinFreq = 80;
    gPmLightSleepRequested = false;
  } else {
    gPmMaxFreq = 80;
    gPmMinFreq = 20;
    gPmLightSleepRequested = true;
  }
#if HAS_ESP_PM && defined(CONFIG_PM_ENABLE)
  gPmCompiled = true;
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  esp_pm_config_esp32s3_t pmConfig = {};
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
  esp_pm_config_esp32s2_t pmConfig = {};
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  esp_pm_config_esp32c3_t pmConfig = {};
#elif defined(CONFIG_IDF_TARGET_ESP32H2)
  esp_pm_config_esp32h2_t pmConfig = {};
#elif defined(CONFIG_IDF_TARGET_ESP32)
  esp_pm_config_esp32_t pmConfig = {};
#else
  esp_pm_config_t pmConfig = {};
#endif
  pmConfig.max_freq_mhz = gPmMaxFreq;
  pmConfig.min_freq_mhz = gPmMinFreq;
  pmConfig.light_sleep_enable = gPmLightSleepRequested;
  gPmConfigureErr = esp_pm_configure(&pmConfig);
  gPmConfigured = (gPmConfigureErr == ESP_OK);
  if (gPmConfigured) {
    Serial.println("PM: enabled DFS/light sleep");
  } else {
    Serial.printf("PM: not available (0x%X)\n", static_cast<unsigned int>(gPmConfigureErr));
  }
#else
  gPmCompiled = false;
  gPmConfigureErr = ESP_FAIL;
  gPmConfigured = false;
  gTxNoLightSleepLockHeld = false;
  Serial.println("PM: not available");
#endif
}

static void printPmStatus() {
  bool txBusy = gWorkerBusy || (txQueue && uxQueueMessagesWaiting(txQueue) > 0);
  Serial.println("PM STATUS:");
  Serial.printf("  compiled: %s\n", gPmCompiled ? "yes" : "no");
  Serial.printf("  configured: %s (err=0x%X)\n", gPmConfigured ? "yes" : "no",
                static_cast<unsigned int>(gPmConfigureErr));
  Serial.printf("  max_freq_mhz: %u\n", static_cast<unsigned int>(gPmMaxFreq));
  Serial.printf("  min_freq_mhz: %u\n", static_cast<unsigned int>(gPmMinFreq));
  Serial.printf("  light_sleep_requested: %s\n", gPmLightSleepRequested ? "true" : "false");
  Serial.printf("  ble_connected: %s\n", gBleConnected ? "true" : "false");
  Serial.printf("  tx_busy: %s\n", txBusy ? "true" : "false");
  Serial.printf("  tx_no_light_sleep_lock_held: %s\n",
                gTxNoLightSleepLockHeld ? "true" : "false");
}

static void bleInit() {
  NimBLEDevice::init("PagerBridge");
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);
  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  NimBLEService *service = server->createService(kBleServiceUuid);
gRxChar = service->createCharacteristic(
    kBleRxUuid, NIMBLE_PROPERTY::WRITE);
  gRxChar->setCallbacks(new RxCallbacks());
  gStatusChar = service->createCharacteristic(kBleStatusUuid,
                                              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  gStatusChar->setValue("READY\n");
  service->start();
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(service->getUUID());
  advertising->setScanResponse(true);
  advertising->setMinInterval(160);
  advertising->setMaxInterval(320);
  advertising->start();
  notifyStatus("READY", true);
}

static void txWorkerTask(void *context) {
  (void)context;
  while (true) {
    TxJob *job = nullptr;
    if (xQueueReceive(txQueue, &job, portMAX_DELAY) != pdTRUE || job == nullptr) {
      continue;
    }
    gWorkerBusy = true;
    bool ok = waveTx.transmitBits(job->bits, job->baud, job->gpio, job->output, job->idleHigh,
                                  job->driveOneLow, config.blockingTx);
    gWorkerBusy = false;
    Serial.println(ok ? "TX_DONE" : "TX_FAIL");
    notifyStatus(ok ? "TX_DONE" : "TX_FAIL", true);
    if (gStatusChar && gLastStatus != "TX_IDLE") {
      notifyStatus("TX_IDLE", false);
    }
    delete job;
  }
}

static void commandWorkerTask(void *context) {
  (void)context;
  while (true) {
    CommandJob *job = nullptr;
    if (xQueueReceive(commandQueue, &job, portMAX_DELAY) != pdTRUE || job == nullptr) {
      continue;
    }
    uint32_t now = millis();
    if (job->notBeforeMs != 0 && static_cast<int32_t>(job->notBeforeMs - now) > 0) {
      vTaskDelay(pdMS_TO_TICKS(job->notBeforeMs - now));
    }
    handleCommand(job->line);
    delete job;
  }
}

static String readFileToString(const char *path) {
  File file = SPIFFS.open(path, "r");
  if (!file) {
    return String();
  }
  String content = file.readString();
  file.close();
  return content;
}

static bool extractJsonValue(const String &json, const char *key, String &out) {
  String needle = String('"') + key + '"';
  int keyPos = json.indexOf(needle);
  if (keyPos < 0) {
    return false;
  }
  int colon = json.indexOf(':', keyPos + needle.length());
  if (colon < 0) {
    return false;
  }
  int start = colon + 1;
  while (start < json.length() && isspace(static_cast<unsigned char>(json[start]))) {
    ++start;
  }
  if (start >= json.length()) {
    return false;
  }
  if (json[start] == '"') {
    int end = json.indexOf('"', start + 1);
    if (end < 0) {
      return false;
    }
    out = json.substring(start + 1, end);
    return true;
  }
  int end = start;
  while (end < json.length() && json[end] != ',' && json[end] != '}' &&
         json[end] != '\n' && json[end] != '\r') {
    ++end;
  }
  out = json.substring(start, end);
  out.trim();
  return true;
}

static bool parseBool(const String &value, bool &out) {
  String normalized = value;
  normalized.toLowerCase();
  if (normalized == "true" || normalized == "1" || normalized == "yes") {
    out = true;
    return true;
  }
  if (normalized == "false" || normalized == "0" || normalized == "no") {
    out = false;
    return true;
  }
  return false;
}

static bool parseOutputMode(const String &value, OutputMode &out) {
  String normalized = value;
  normalized.toLowerCase();
  if (normalized == "open_drain") {
    out = OutputMode::kOpenDrain;
    return true;
  }
  if (normalized == "push_pull") {
    out = OutputMode::kPushPull;
    return true;
  }
  return false;
}

static void applyIdlePins() {
  if (config.output == OutputMode::kOpenDrain && config.idleHigh) {
    pinMode(config.dataGpio, OUTPUT_OPEN_DRAIN);
    digitalWrite(config.dataGpio, HIGH);
    return;
  }
  pinMode(config.dataGpio, INPUT);
  gpio_pullup_dis(static_cast<gpio_num_t>(config.dataGpio));
  gpio_pulldown_dis(static_cast<gpio_num_t>(config.dataGpio));
}

static bool applyPreset(const String &name, bool report) {
  String normalized = name;
  normalized.toLowerCase();
  if (normalized == "advisor" || normalized == "pager") {
    config.baud = 512;
    config.preambleBits = 576;
    config.capInd = 1422890;
    config.capGrp = 1422890;
    config.functionBits = 2;
    config.dataGpio = 4;
    config.output = OutputMode::kOpenDrain;
    config.invertWords = false;
    config.driveOneLow = true;
    config.idleHigh = true;
  } else if (normalized == "generic" || normalized == "lora_baseline") {
    config.baud = 512;
    config.preambleBits = 1024;
    config.capInd = 1422890;
    config.capGrp = 1422890;
    config.functionBits = 0;
    config.dataGpio = 4;
    config.output = OutputMode::kPushPull;
    config.invertWords = false;
    config.driveOneLow = false;
    config.idleHigh = true;
  } else {
    if (report) {
      Serial.println("ERR: preset must be ADVISOR|GENERIC");
    }
    return false;
  }
  applyIdlePins();
  if (report) {
    Serial.printf("OK PRESET %s\n", normalized.c_str());
    printStatus();
  }
  return true;
}

static void writeConfigFile() {
  File file = SPIFFS.open(kConfigPath, "w");
  if (!file) {
    Serial.println("ERR: failed to open config for write");
    return;
  }
  String outputMode = (config.output == OutputMode::kOpenDrain) ? "open_drain" : "push_pull";
  file.println("{");
  file.printf("  \"baud\": %u,\n", config.baud);
  file.printf("  \"preambleBits\": %u,\n", config.preambleBits);
  file.printf("  \"capInd\": %u,\n", config.capInd);
  file.printf("  \"capGrp\": %u,\n", config.capGrp);
  file.printf("  \"functionBits\": %u,\n", config.functionBits);
  file.printf("  \"dataGpio\": %d,\n", config.dataGpio);
  file.printf("  \"output\": \"%s\",\n", outputMode.c_str());
  file.printf("  \"invertWords\": %s,\n", config.invertWords ? "true" : "false");
  file.printf("  \"driveOneLow\": %s,\n", config.driveOneLow ? "true" : "false");
  file.printf("  \"idleHigh\": %s,\n", config.idleHigh ? "true" : "false");
  file.printf("  \"blockingTx\": %s,\n", config.blockingTx ? "true" : "false");
  file.printf("  \"bootPreset\": \"%s\",\n", config.bootPreset.c_str());
  file.printf("  \"firstBootShown\": %s\n", config.firstBootShown ? "true" : "false");
  file.println("}");
  file.close();
}

static bool loadConfig() {
  config = Config();
  bool shouldShowHelp = false;
  bool sawNewPreambleBits = false;
  bool sawLegacyPreambleMs = false;
  uint32_t legacyPreambleMs = 0;
  bool sawLegacyInvert = false;
  bool legacyInvert = false;
  if (!SPIFFS.begin(true)) {
    Serial.println("ERR: SPIFFS mount failed");
    return false;
  }
  if (!SPIFFS.exists(kConfigPath)) {
    shouldShowHelp = true;
    return shouldShowHelp;
  }
  String json = readFileToString(kConfigPath);
  String value;

  if (extractJsonValue(json, "baud", value)) {
    uint32_t baud = static_cast<uint32_t>(value.toInt());
    if (baud == 512 || baud == 1200 || baud == 2400) {
      config.baud = baud;
    }
  }
  if (extractJsonValue(json, "preambleBits", value)) {
    config.preambleBits = static_cast<uint32_t>(value.toInt());
    sawNewPreambleBits = true;
  }
  if (extractJsonValue(json, "preambleMs", value)) {
    legacyPreambleMs = static_cast<uint32_t>(value.toInt());
    sawLegacyPreambleMs = true;
  }
  if (extractJsonValue(json, "idleHigh", value)) {
    parseBool(value, config.idleHigh);
  }
  if (extractJsonValue(json, "blockingTx", value)) {
    parseBool(value, config.blockingTx);
  }
  if (extractJsonValue(json, "output", value)) {
    parseOutputMode(value, config.output);
  }
  if (extractJsonValue(json, "invertWords", value)) {
    parseBool(value, config.invertWords);
  }
  if (extractJsonValue(json, "driveOneLow", value)) {
    parseBool(value, config.driveOneLow);
  }
  if (extractJsonValue(json, "invert", value)) {
    parseBool(value, legacyInvert);
    sawLegacyInvert = true;
  }
  if (extractJsonValue(json, "bootPreset", value)) {
    if (value.length() > 0) {
      config.bootPreset = value;
    }
  }
  if (extractJsonValue(json, "capInd", value)) {
    config.capInd = static_cast<uint32_t>(value.toInt());
  }
  if (extractJsonValue(json, "capGrp", value)) {
    config.capGrp = static_cast<uint32_t>(value.toInt());
  }
  if (extractJsonValue(json, "functionBits", value)) {
    config.functionBits = static_cast<uint8_t>(value.toInt() & 0x3);
  }
  if (extractJsonValue(json, "dataGpio", value)) {
    int dataGpio = value.toInt();
    if (dataGpio > 0) {
      config.dataGpio = dataGpio;
    } else {
      config.dataGpio = 4;
    }
  }
  if (extractJsonValue(json, "firstBootShown", value)) {
    parseBool(value, config.firstBootShown);
  }
  if (!sawNewPreambleBits && sawLegacyPreambleMs) {
    config.preambleBits = (legacyPreambleMs * config.baud) / 1000;
  }
  if (sawLegacyInvert) {
    config.driveOneLow = legacyInvert;
  }
  shouldShowHelp = !config.firstBootShown;
  return shouldShowHelp;
}

static void printStatus() {
  String outputMode = (config.output == OutputMode::kOpenDrain) ? "open_drain" : "push_pull";
  uint32_t bitPeriodUs = (1000000 + (config.baud / 2)) / config.baud;
  Serial.printf(
      "baud=%u preambleBits=%u capInd=%u capGrp=%u functionBits=%u dataGpio=%d "
      "output=%s invertWords=%s driveOneLow=%s idleHigh=%s bootPreset=%s\n",
      config.baud, config.preambleBits, config.capInd, config.capGrp, config.functionBits,
      config.dataGpio, outputMode.c_str(), config.invertWords ? "true" : "false",
      config.driveOneLow ? "true" : "false", config.idleHigh ? "true" : "false",
      config.bootPreset.c_str());
  Serial.printf("line: gpio=%d mode=%s idle=%s bitPeriodUs=%u\n", config.dataGpio,
                outputMode.c_str(), config.idleHigh ? "HIGH" : "LOW", bitPeriodUs);
}

static std::vector<uint8_t> buildAlternatingBits(uint32_t durationMs, uint32_t baud) {
  uint32_t bitCount = (durationMs * baud) / 1000;
  if (bitCount == 0) {
    bitCount = 1;
  }
  std::vector<uint8_t> bits;
  bits.reserve(bitCount);
  for (uint32_t i = 0; i < bitCount; ++i) {
    bits.push_back(static_cast<uint8_t>(i % 2 == 0));
  }
  return bits;
}

static std::vector<uint8_t> buildIdleBits(uint32_t durationMs, uint32_t baud, bool driveOneLow,
                                          bool idleHigh) {
  uint32_t bitCount = (durationMs * baud) / 1000;
  if (bitCount == 0) {
    bitCount = 1;
  }
  uint8_t idleBit = 0;
  if (driveOneLow) {
    idleBit = idleHigh ? 0 : 1;
  } else {
    idleBit = idleHigh ? 1 : 0;
  }
  return std::vector<uint8_t>(bitCount, idleBit);
}

static std::vector<uint8_t> buildSyncAndBatchBits(const std::vector<uint32_t> &words,
                                                  bool invertWords) {
  std::vector<uint8_t> bits;
  bits.reserve(544);
  uint32_t syncWord = invertWords ? ~kSyncWord : kSyncWord;
  for (int i = 31; i >= 0; --i) {
    bits.push_back(static_cast<uint8_t>((syncWord >> i) & 0x1));
  }
  for (uint32_t word : words) {
    for (int i = 31; i >= 0; --i) {
      bits.push_back(static_cast<uint8_t>((word >> i) & 0x1));
    }
  }
  return bits;
}

static void applyWordInversion(std::vector<uint32_t> &words, bool invertWords) {
  if (!invertWords) {
    return;
  }
  for (uint32_t &word : words) {
    word = ~word;
  }
}

static std::vector<uint32_t> buildMinimalBatch(uint32_t capcode, uint8_t functionBits) {
  std::vector<uint32_t> words(16, kIdleWord);
  uint8_t frame = static_cast<uint8_t>(capcode & 0x7);
  uint32_t addressWord = encoder.buildAddressCodeword(capcode, functionBits);
  size_t index = static_cast<size_t>(frame) * 2;
  if (index < words.size()) {
    words[index] = addressWord;
  }
  return words;
}

static uint32_t computeRepeatBatches(uint32_t repeatMs, uint32_t baud) {
  uint64_t totalBits = static_cast<uint64_t>(repeatMs) * static_cast<uint64_t>(baud);
  uint32_t totalBatches = static_cast<uint32_t>(totalBits / (1000ULL * 544ULL));
  if (totalBatches == 0) {
    totalBatches = 1;
  }
  return totalBatches;
}

static std::vector<uint8_t> buildPocsagBits(const String &message, uint32_t capcode,
                                            uint32_t preambleBits) {
  std::vector<uint8_t> bits;
  if (preambleBits > 0) {
    bits.reserve(preambleBits);
    for (uint32_t i = 0; i < preambleBits; ++i) {
      bits.push_back(static_cast<uint8_t>(i % 2 == 0));
    }
  }

  std::vector<std::vector<uint32_t>> batches =
      encoder.buildBatchWords(capcode, config.functionBits, message);
  bits.reserve(bits.size() + (batches.size() * 544));

  for (std::vector<uint32_t> &batch : batches) {
    applyWordInversion(batch, config.invertWords);
    std::vector<uint8_t> batchBits = buildSyncAndBatchBits(batch, config.invertWords);
    bits.insert(bits.end(), batchBits.begin(), batchBits.end());
  }

  return bits;
}

[[maybe_unused]] static std::vector<uint8_t> buildRepeatedPocsagBits(const String &message,
                                                                     uint32_t capcode,
                                                                     uint32_t preambleBits,
                                                                     uint32_t repeatMs) {
  std::vector<uint8_t> bits;
  if (preambleBits > 0) {
    bits.reserve(preambleBits);
    for (uint32_t i = 0; i < preambleBits; ++i) {
      bits.push_back(static_cast<uint8_t>(i % 2 == 0));
    }
  }

  std::vector<uint8_t> oneMessageBits;
  std::vector<std::vector<uint32_t>> batches =
      encoder.buildBatchWords(capcode, config.functionBits, message);
  oneMessageBits.reserve(batches.size() * 544);
  for (std::vector<uint32_t> &batch : batches) {
    applyWordInversion(batch, config.invertWords);
    std::vector<uint8_t> batchBits = buildSyncAndBatchBits(batch, config.invertWords);
    oneMessageBits.insert(oneMessageBits.end(), batchBits.begin(), batchBits.end());
  }

  uint32_t totalBatches = computeRepeatBatches(repeatMs, config.baud);
  bits.reserve(bits.size() + (oneMessageBits.size() * totalBatches));
  for (uint32_t i = 0; i < totalBatches; ++i) {
    bits.insert(bits.end(), oneMessageBits.begin(), oneMessageBits.end());
  }
  return bits;
}

[[maybe_unused]] static std::vector<uint8_t> buildMinimalPocsagBits(uint32_t capcode,
                                                                    uint8_t functionBits,
                                                                    uint32_t preambleMs) {
  std::vector<uint8_t> bits = buildAlternatingBits(preambleMs, config.baud);
  std::vector<uint32_t> words = buildMinimalBatch(capcode, functionBits);
  applyWordInversion(words, config.invertWords);
  std::vector<uint8_t> batchBits = buildSyncAndBatchBits(words, config.invertWords);
  bits.insert(bits.end(), batchBits.begin(), batchBits.end());
  return bits;
}

static std::vector<uint8_t> buildRepeatedMinimalBits(uint32_t capcode, uint8_t functionBits,
                                                     uint32_t preambleMs, uint32_t repeatMs) {
  std::vector<uint8_t> bits = buildAlternatingBits(preambleMs, config.baud);
  std::vector<uint32_t> words = buildMinimalBatch(capcode, functionBits);
  applyWordInversion(words, config.invertWords);
  std::vector<uint8_t> batchBits = buildSyncAndBatchBits(words, config.invertWords);
  uint32_t totalBatches = computeRepeatBatches(repeatMs, config.baud);
  bits.reserve(bits.size() + (batchBits.size() * totalBatches));
  for (uint32_t i = 0; i < totalBatches; ++i) {
    bits.insert(bits.end(), batchBits.begin(), batchBits.end());
  }
  return bits;
}

static bool queueTxJob(const std::vector<uint8_t> &bits) {
  if (!txQueue) {
    Serial.println("ERR: TX_QUEUE");
    return false;
  }
  if (gWorkerBusy || uxQueueSpacesAvailable(txQueue) == 0) {
    Serial.println("TX_BUSY");
    notifyStatus("TX_BUSY", true);
    return false;
  }
  TxJob *job = new TxJob{bits, config.baud, config.dataGpio, config.output, config.idleHigh,
                         config.driveOneLow};
  if (xQueueSend(txQueue, &job, 0) != pdTRUE) {
    delete job;
    Serial.println("TX_BUSY");
    notifyStatus("TX_BUSY", true);
    return false;
  }
  Serial.println("QUEUED");
  notifyStatus("TX_BUSY", true);
  return true;
}

static bool isLikelyMessageId(const String &token) {
  if (token.startsWith("id:") || token.startsWith("ID:")) {
    return true;
  }
  if (token.length() == 0 || token.length() > 24) {
    return false;
  }
  for (size_t i = 0; i < token.length(); ++i) {
    if (!isAlphaNumeric(static_cast<unsigned char>(token[i])) && token[i] != '-' &&
        token[i] != '_') {
      return false;
    }
  }
  bool hasDigit = false;
  for (size_t i = 0; i < token.length(); ++i) {
    if (isDigit(static_cast<unsigned char>(token[i]))) {
      hasDigit = true;
      break;
    }
  }
  return hasDigit;
}

static String normalizeWhitespaceLower(const String &input) {
  String out;
  bool prevSpace = false;
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    if (isspace(static_cast<unsigned char>(c))) {
      if (!prevSpace && out.length() > 0) {
        out += ' ';
      }
      prevSpace = true;
      continue;
    }
    prevSpace = false;
    out += static_cast<char>(tolower(static_cast<unsigned char>(c)));
  }
  out.trim();
  return out;
}

static bool sendMessageOnce(const String &message, uint32_t capcode) {
  std::vector<uint8_t> bits = buildPocsagBits(message, capcode, config.preambleBits);
  return queueTxJob(bits);
}

static void handleScope(uint32_t durationMs) {
  std::vector<uint8_t> bits = buildAlternatingBits(durationMs, config.baud);
  queueTxJob(bits);
}

static void handleDebugScope() {
  std::vector<uint8_t> bits = buildAlternatingBits(2000, config.baud);
  std::vector<uint8_t> idleBits =
      buildIdleBits(2000, config.baud, config.driveOneLow, config.idleHigh);
  bits.insert(bits.end(), idleBits.begin(), idleBits.end());
  queueTxJob(bits);
}

static void handleSendMin(uint32_t capcode, uint8_t functionBits, uint32_t repeatMs) {
  std::vector<uint8_t> bits = buildRepeatedMinimalBits(capcode, functionBits, 2000, repeatMs);
  queueTxJob(bits);
}

static void handleDumpMin(uint32_t capcode, uint8_t functionBits) {
  uint8_t frame = static_cast<uint8_t>(capcode & 0x7);
  uint32_t addressWord = encoder.buildAddressCodeword(capcode, functionBits);
  std::vector<uint32_t> words = buildMinimalBatch(capcode, functionBits);
  if (config.invertWords) {
    addressWord = ~addressWord;
  }
  applyWordInversion(words, config.invertWords);
  Serial.printf("frame=%u\n", frame);
  Serial.printf("address=0x%08lX\n", static_cast<unsigned long>(addressWord));
  for (size_t i = 0; i < words.size(); ++i) {
    Serial.printf("batch[%u]=0x%08lX\n", static_cast<unsigned int>(i),
                  static_cast<unsigned long>(words[i]));
  }
}

static void runMessageLoop(uint32_t seconds, const String &message, uint32_t capcode) {
  uint32_t start = millis();
  while ((millis() - start) < (seconds * 1000UL)) {
    if (!sendMessageOnce(message, capcode)) {
      delay(5);
    }
  }
}

static void applySetCommand(const String &key, const String &value) {
  String normalizedKey = key;
  normalizedKey.toLowerCase();

  if (normalizedKey == "baud") {
    uint32_t baud = static_cast<uint32_t>(value.toInt());
    if (baud == 512 || baud == 1200 || baud == 2400) {
      config.baud = baud;
    }
  } else if (normalizedKey == "preamblebits") {
    config.preambleBits = static_cast<uint32_t>(value.toInt());
  } else if (normalizedKey == "idlehigh") {
    parseBool(value, config.idleHigh);
  } else if (normalizedKey == "output") {
    parseOutputMode(value, config.output);
  } else if (normalizedKey == "invertwords") {
    parseBool(value, config.invertWords);
  } else if (normalizedKey == "driveonelow") {
    parseBool(value, config.driveOneLow);
  } else if (normalizedKey == "blockingtx") {
    parseBool(value, config.blockingTx);
  } else if (normalizedKey == "capind") {
    config.capInd = static_cast<uint32_t>(value.toInt());
  } else if (normalizedKey == "capgrp") {
    config.capGrp = static_cast<uint32_t>(value.toInt());
  } else if (normalizedKey == "functionbits") {
    config.functionBits = static_cast<uint8_t>(value.toInt() & 0x3);
  } else if (normalizedKey == "datagpio") {
    int dataGpio = value.toInt();
    if (dataGpio > 0) {
      config.dataGpio = dataGpio;
    } else {
      config.dataGpio = 4;
    }
  } else {
    Serial.println("ERR: unknown key");
    return;
  }

  applyIdlePins();
  Serial.println("OK");
}

static void printHelp() {
  Serial.println("POCSAG TX (RMT) Commands:");
  Serial.println(
      "  STATUS | PM | PM_STATUS | HELP | ? | PRESET <name> | SCOPE <ms> | DEBUG_SCOPE | H | SEND <text> | "
      "SEND_MIN <capcode> [func] [repeatMs] | DUMP_MIN <capcode> <func> | STATUS_TX | T1 <sec>");
  Serial.println("  SET <key> <value> | SAVE | LOAD");
  Serial.println("Presets: ADVISOR | GENERIC");
  Serial.println("SET keys: baud preambleBits capInd capGrp functionBits dataGpio output");
  Serial.println("          invertWords driveOneLow idleHigh blockingTx");
  Serial.println("Examples:");
  Serial.println("  PRESET ADVISOR");
  Serial.println("  STATUS");
  Serial.println("  SCOPE 2000");
  Serial.println("  DEBUG_SCOPE");
  Serial.println("  H");
  Serial.println("  SEND HELLO WORLD");
  Serial.println("  SEND_MIN 1422890 2 4000");
  Serial.println("  STATUS_TX");
  Serial.println("  DUMP_MIN 1422890 2");
  Serial.println("  T1 10");
  Serial.println(
      "ADVISOR preset: baud=512 invertWords=false driveOneLow=true idleHigh=true output=open_drain");
  Serial.printf(
      "Defaults: baud=%u preambleBits=%u output=%s dataGpio=%d invertWords=%s driveOneLow=%s idleHigh=%s\n",
      config.baud, config.preambleBits,
      (config.output == OutputMode::kOpenDrain) ? "open_drain" : "push_pull",
      config.dataGpio, config.invertWords ? "true" : "false",
      config.driveOneLow ? "true" : "false", config.idleHigh ? "true" : "false");
}

static void handleCommand(const String &line) {
  String trimmed = line;
  trimmed.trim();
  if (trimmed.length() == 0) {
    return;
  }
  int space = trimmed.indexOf(' ');
  String cmd = (space < 0) ? trimmed : trimmed.substring(0, space);
  cmd.toUpperCase();

  if (cmd == "SEND") {
    String msgOnly = (space < 0) ? "" : trimmed.substring(space + 1);
    msgOnly.trim();
    int secondSpace = msgOnly.indexOf(' ');
    String firstToken = (secondSpace < 0) ? msgOnly : msgOnly.substring(0, secondSpace);
    String remainder = (secondSpace < 0) ? "" : msgOnly.substring(secondSpace + 1);
    remainder.trim();

    String key;
    if (secondSpace > 0 && remainder.length() > 0 && isLikelyMessageId(firstToken)) {
      key = String("id:") + normalizeWhitespaceLower(firstToken);
    } else {
      key = String("msg:") + normalizeWhitespaceLower(msgOnly);
    }
    uint32_t now = millis();
    if (key == gLastSendLine && (now - gLastSendLineMs) < kSendDedupeWindowMs) {
      Serial.println("DROP_DUP_SEND");
      notifyStatus("DROP_DUP", true);
      return;
    }
    gLastSendLine = key;
    gLastSendLineMs = now;
  }

  if (cmd == "STATUS") {
    printStatus();
    return;
  }
  if (cmd == "PM") {
    printPmStatus();
    return;
  }
  if (cmd == "PM_STATUS") {
    printPmStatus();
    return;
  }
  if (cmd == "PING") {
    notifyStatus("CONNECTED", true);
    Serial.println("PONG");
    return;
  }
  if (cmd == "STATUS_TX") {
    bool busy = gWorkerBusy || (txQueue && uxQueueMessagesWaiting(txQueue) > 0);
    Serial.println(busy ? "TX_BUSY" : "TX_IDLE");
    return;
  }
  if (cmd == "SAVE") {
    writeConfigFile();
    Serial.println("SAVED");
    return;
  }
  if (cmd == "LOAD") {
    loadConfig();
    applyIdlePins();
    Serial.println("LOADED");
    return;
  }
  if (cmd == "H") {
    sendMessageOnce("H", config.capInd);
    return;
  }
  if (cmd == "SCOPE") {
    String value = (space < 0) ? "" : trimmed.substring(space + 1);
    handleScope(static_cast<uint32_t>(value.toInt()));
    return;
  }
  if (cmd == "DEBUG_SCOPE") {
    handleDebugScope();
    return;
  }
  if (cmd == "SEND") {
    String message = (space < 0) ? "" : trimmed.substring(space + 1);
    message.trim();
    if (message.length() == 0) {
      Serial.println("ERR: SEND <text>");
      return;
    }
    sendMessageOnce(message, config.capInd);
    return;
  }
  if (cmd == "SEND_MIN") {
    if (space < 0) {
      Serial.println("ERR: SEND_MIN <capcode> [func] [repeatMs]");
      return;
    }
    int secondSpace = trimmed.indexOf(' ', space + 1);
    String capcodeValue =
        (secondSpace < 0) ? trimmed.substring(space + 1) : trimmed.substring(space + 1, secondSpace);
    uint32_t capcode = static_cast<uint32_t>(capcodeValue.toInt());
    uint8_t functionBits = 2;
    uint32_t repeatMs = 4000;
    if (secondSpace >= 0) {
      int thirdSpace = trimmed.indexOf(' ', secondSpace + 1);
      String funcValue =
          (thirdSpace < 0) ? trimmed.substring(secondSpace + 1)
                           : trimmed.substring(secondSpace + 1, thirdSpace);
      functionBits = static_cast<uint8_t>(funcValue.toInt() & 0x3);
      if (thirdSpace >= 0) {
        repeatMs = static_cast<uint32_t>(trimmed.substring(thirdSpace + 1).toInt());
      }
    }
    handleSendMin(capcode, functionBits, repeatMs);
    return;
  }
  if (cmd == "DUMP_MIN") {
    if (space < 0) {
      Serial.println("ERR: DUMP_MIN <capcode> <func>");
      return;
    }
    int secondSpace = trimmed.indexOf(' ', space + 1);
    if (secondSpace < 0) {
      Serial.println("ERR: DUMP_MIN <capcode> <func>");
      return;
    }
    uint32_t capcode = static_cast<uint32_t>(trimmed.substring(space + 1, secondSpace).toInt());
    uint8_t functionBits = static_cast<uint8_t>(trimmed.substring(secondSpace + 1).toInt() & 0x3);
    handleDumpMin(capcode, functionBits);
    return;
  }
  if (cmd == "T1") {
    String value = (space < 0) ? "" : trimmed.substring(space + 1);
    runMessageLoop(static_cast<uint32_t>(value.toInt()), "HELLO WORLD", config.capInd);
    return;
  }
  if (cmd == "PRESET") {
    String value = (space < 0) ? "" : trimmed.substring(space + 1);
    value.trim();
    applyPreset(value, true);
    return;
  }
  if (cmd == "HELP" || cmd == "?") {
    printHelp();
    return;
  }
  if (cmd == "SET") {
    int secondSpace = trimmed.indexOf(' ', space + 1);
    if (space < 0 || secondSpace < 0) {
      Serial.println("ERR: SET <key> <value>");
      return;
    }
    String key = trimmed.substring(space + 1, secondSpace);
    String value = trimmed.substring(secondSpace + 1);
    applySetCommand(key, value);
    return;
  }
  Serial.println("ERR: unknown command");
}

static inline void setUserLed(bool on) {
  digitalWrite(kUserLedPin, on ? LOW : HIGH);
}

static void ledTask(void *context) {
  (void)context;
  // Active-low LED: ON at boot for 5s, then short heartbeat pulse every 15s.
  setUserLed(true);
  vTaskDelay(kBootLedOnTicks);
  setUserLed(false);
  TickType_t lastWake = xTaskGetTickCount();
  while (true) {
    vTaskDelayUntil(&lastWake, kHeartbeatPeriodTicks);
    setUserLed(true);
    vTaskDelay(kHeartbeatPulseTicks);
    setUserLed(false);
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // USER LED on XIAO ESP32-S3 is active-low. Keep OFF by default.
  pinMode(kUserLedPin, OUTPUT);
  setUserLed(false);

  loadConfig();
  setupPowerManagement();
  if (!applyPreset(config.bootPreset, false)) {
    applyIdlePins();
  }
  applyIdlePins();

  txQueue = xQueueCreate(4, sizeof(TxJob *));
  commandQueue = xQueueCreate(8, sizeof(CommandJob *));
  if (txQueue) {
    xTaskCreatePinnedToCore(txWorkerTask, "txWorker", 8192, nullptr, 1, nullptr, 0);
  } else {
    Serial.println("ERR: TX_QUEUE_CREATE");
  }
  if (commandQueue) {
    xTaskCreatePinnedToCore(commandWorkerTask, "cmdWorker", 6144, nullptr, 1, nullptr, 1);
  } else {
    Serial.println("ERR: CMD_QUEUE_CREATE");
  }

  xTaskCreatePinnedToCore(ledTask, "ledTask", 2048, nullptr, 1, nullptr, 1);

  // Bring up BLE only after queues/tasks exist so first reconnect write is not lost.
  bleInit();
  printPmStatus();
  printStatus();
  printHelp();
  if (!config.firstBootShown) {
    config.firstBootShown = true;
    writeConfigFile();
  }
}

// Manual acceptance tests:
// 1) Verify repeated SEND commands (20+) do not hang and do not overlap (no TX_BUSY stuck forever).
// 2) Verify that after TX_DONE/TX_FAIL the data GPIO is Hi-Z (idle).
// 3) Verify BLE reconnect and immediate write is not dropped.
// 4) Verify LED still does 5s solid then 15s heartbeat pulse.
void loop() {
  static String lineBuffer;
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (lineBuffer.length() > 0) {
        enqueueCommand(lineBuffer);
        lineBuffer = "";
      }
    } else {
      lineBuffer += c;
    }
  }
  vTaskDelay(pdMS_TO_TICKS(1000));
}

#if !defined(CONFIG_AUTOSTART_ARDUINO) || !CONFIG_AUTOSTART_ARDUINO
namespace {
TaskHandle_t gArduinoLoopTaskHandle = nullptr;

void arduinoLoopTask(void *context) {
  (void)context;
  setup();
  while (true) {
    loop();
    if (serialEventRun) {
      serialEventRun();
    }
  }
}
}

extern "C" void app_main() {
  initArduino();
  xTaskCreateUniversal(arduinoLoopTask, "loopTask", 8192, nullptr, 1,
                       &gArduinoLoopTaskHandle, ARDUINO_RUNNING_CORE);
}
#endif
