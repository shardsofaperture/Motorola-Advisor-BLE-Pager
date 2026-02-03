#include <Arduino.h>
#include <LittleFS.h>

#include <vector>

namespace {
constexpr const char *kConfigPath = "/config.json";
constexpr uint32_t kSyncWord = 0x7CD215D8;
constexpr uint32_t kIdleWord = 0x7A89C197;
constexpr uint32_t kDefaultIntervalMs = 250;
}

enum class OutputMode : uint8_t { kOpenDrain = 0, kPushPull = 1 };

struct Config {
  uint32_t baud = 512;
  bool invert = false;
  bool idleHigh = true;
  OutputMode output = OutputMode::kOpenDrain;
  uint32_t preambleMs = 2000;
  uint32_t capInd = 1422890;
  uint32_t capGrp = 1422890;
  uint8_t functionBits = 0;
  int dataGpio = 3;
  int rfSenseGpio = -1;
};

static Config config;

static hw_timer_t *txTimer = nullptr;
static portMUX_TYPE txMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool txActive = false;
static volatile size_t txIndex = 0;
static const uint8_t *txData = nullptr;
static size_t txSize = 0;
static bool txInvert = false;
static bool txIdleHigh = true;
static OutputMode txOutput = OutputMode::kOpenDrain;
static int txGpio = 3;

static volatile uint32_t rfSenseCount = 0;
static volatile uint32_t rfSenseLastMs = 0;
static volatile uint64_t rfSensePeriodTotal = 0;
static volatile uint32_t rfSensePeriodCount = 0;

class PocsagEncoder {
 public:
  std::vector<uint8_t> buildBitstream(uint32_t capcode, uint8_t functionBits,
                                      const String &message, uint32_t preambleMs,
                                      uint32_t baud) const {
    std::vector<uint8_t> bits;
    appendPreamble(bits, preambleMs, baud);
    appendWord(bits, kSyncWord);
    std::vector<uint32_t> batch = buildSingleBatch(capcode, functionBits, message);
    for (uint32_t word : batch) {
      appendWord(bits, word);
    }
    return bits;
  }

 private:
  void appendPreamble(std::vector<uint8_t> &bits, uint32_t preambleMs,
                      uint32_t baud) const {
    uint32_t preambleBits = (preambleMs * baud) / 1000;
    bits.reserve(bits.size() + preambleBits + 32 * 17);
    for (uint32_t i = 0; i < preambleBits; ++i) {
      bits.push_back(static_cast<uint8_t>(i % 2 == 0));
    }
  }

  void appendWord(std::vector<uint8_t> &bits, uint32_t word) const {
    for (int i = 31; i >= 0; --i) {
      bits.push_back(static_cast<uint8_t>((word >> i) & 0x1));
    }
  }

  std::vector<uint32_t> buildSingleBatch(uint32_t capcode, uint8_t functionBits,
                                         const String &message) const {
    std::vector<uint32_t> words(16, kIdleWord);
    uint8_t frame = static_cast<uint8_t>(capcode & 0x7);
    uint32_t addressWord = buildAddressWord(capcode, functionBits);
    std::vector<uint32_t> messageWords = buildAlphaWords(message);

    size_t index = frame * 2;
    if (index < words.size()) {
      words[index++] = addressWord;
    }
    for (uint32_t word : messageWords) {
      if (index >= words.size()) {
        break;
      }
      words[index++] = word;
    }
    return words;
  }

  std::vector<uint32_t> buildAlphaWords(const String &message) const {
    std::vector<uint8_t> bits;
    bits.reserve(message.length() * 7);
    for (size_t i = 0; i < message.length(); ++i) {
      uint8_t value = static_cast<uint8_t>(message[i]) & 0x7F;
      for (int b = 0; b <= 6; ++b) {
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
    return buildCodeword(0, data);
  }

  uint32_t buildMessageWord(uint32_t data20) const {
    return buildCodeword(1, data20 & 0xFFFFF);
  }

  uint32_t buildCodeword(uint8_t typeBit, uint32_t data) const {
    uint32_t data21 = (static_cast<uint32_t>(typeBit) << 20) | (data & 0xFFFFF);
    uint32_t bch = bchEncode(data21);
    uint32_t word = (data21 << 11) | (bch << 1);
    uint32_t parity = computeParity(word);
    return word | parity;
  }

  uint32_t bchEncode(uint32_t data21) const {
    uint32_t reg = data21 << 10;
    constexpr uint32_t poly = 0x3B9;
    for (int i = 31; i >= 10; --i) {
      if (reg & (1u << i)) {
        reg ^= (poly << (i - 10));
      }
    }
    return reg & 0x3FF;
  }

  uint32_t computeParity(uint32_t value) const {
    uint32_t parity = 0;
    while (value) {
      parity ^= (value & 1u);
      value >>= 1;
    }
    return parity & 0x1;
  }
};

static PocsagEncoder encoder;
static std::vector<uint8_t> txBits;

static void applyLineState(bool logicalHigh) {
  bool bit = txInvert ? !logicalHigh : logicalHigh;
  if (txOutput == OutputMode::kOpenDrain) {
    if (bit) {
      pinMode(txGpio, INPUT);
    } else {
      pinMode(txGpio, OUTPUT);
      digitalWrite(txGpio, LOW);
    }
  } else {
    pinMode(txGpio, OUTPUT);
    digitalWrite(txGpio, bit ? HIGH : LOW);
  }
}

static void IRAM_ATTR onTxTimer() {
  portENTER_CRITICAL_ISR(&txMux);
  if (!txActive || txIndex >= txSize) {
    txActive = false;
    if (txTimer) {
      timerAlarmDisable(txTimer);
    }
    portEXIT_CRITICAL_ISR(&txMux);
    return;
  }
  bool bit = txData[txIndex++] != 0;
  portEXIT_CRITICAL_ISR(&txMux);
  applyLineState(bit);
  if (txIndex >= txSize) {
    txActive = false;
    if (txTimer) {
      timerAlarmDisable(txTimer);
    }
  }
}

static void beginTxTimer() {
  if (txTimer) {
    return;
  }
  txTimer = timerBegin(0, 80, true);
  timerAttachInterrupt(txTimer, &onTxTimer, true);
}

static void setIdleLine() {
  applyLineState(txIdleHigh);
}

static bool startTx(const std::vector<uint8_t> &bits, uint32_t baud, bool invert,
                    bool idleHigh, OutputMode outputMode, int dataGpio) {
  if (txActive) {
    return false;
  }
  if (bits.empty()) {
    return false;
  }
  beginTxTimer();
  txBits = bits;
  txData = txBits.data();
  txSize = txBits.size();
  txIndex = 0;
  txInvert = invert;
  txIdleHigh = idleHigh;
  txOutput = outputMode;
  txGpio = dataGpio;
  uint32_t periodUs = 1000000UL / baud;
  timerAlarmWrite(txTimer, periodUs, true);
  txActive = true;
  timerAlarmEnable(txTimer);
  return true;
}

static bool isTxActive() { return txActive; }

static void waitForTxComplete() {
  while (txActive) {
    delay(1);
  }
  setIdleLine();
}

static String readFileToString(const char *path) {
  File file = LittleFS.open(path, "r");
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

static void applyRfSenseConfig() {
  if (config.rfSenseGpio < 0) {
    return;
  }
  pinMode(config.rfSenseGpio, INPUT);
}

static void IRAM_ATTR onRfSense() {
  uint32_t now = millis();
  rfSenseCount++;
  if (rfSenseLastMs > 0) {
    rfSensePeriodTotal += (now - rfSenseLastMs);
    rfSensePeriodCount++;
  }
  rfSenseLastMs = now;
}

static void attachRfSense() {
  if (config.rfSenseGpio < 0) {
    return;
  }
  detachInterrupt(config.rfSenseGpio);
  pinMode(config.rfSenseGpio, INPUT);
  attachInterrupt(config.rfSenseGpio, &onRfSense, RISING);
}

static void detachRfSenseIfNeeded(int oldGpio) {
  if (oldGpio >= 0 && oldGpio != config.rfSenseGpio) {
    detachInterrupt(oldGpio);
  }
}

static void applyConfigPins() {
  txInvert = config.invert;
  txIdleHigh = config.idleHigh;
  txOutput = config.output;
  txGpio = config.dataGpio;
  setIdleLine();
  applyRfSenseConfig();
}

static void writeConfigFile() {
  File file = LittleFS.open(kConfigPath, "w");
  if (!file) {
    Serial.println("ERR: failed to open config for write");
    return;
  }
  String outputMode = (config.output == OutputMode::kOpenDrain) ? "open_drain" : "push_pull";
  file.println("{");
  file.printf("  \"baud\": %u,\n", config.baud);
  file.printf("  \"invert\": %s,\n", config.invert ? "true" : "false");
  file.printf("  \"idleHigh\": %s,\n", config.idleHigh ? "true" : "false");
  file.printf("  \"output\": \"%s\",\n", outputMode.c_str());
  file.printf("  \"preambleMs\": %u,\n", config.preambleMs);
  file.printf("  \"capInd\": %u,\n", config.capInd);
  file.printf("  \"capGrp\": %u,\n", config.capGrp);
  file.printf("  \"functionBits\": %u,\n", config.functionBits);
  file.printf("  \"dataGpio\": %d,\n", config.dataGpio);
  file.printf("  \"rfSenseGpio\": %d\n", config.rfSenseGpio);
  file.println("}");
  file.close();
}

static void loadConfig() {
  config = Config();
  if (!LittleFS.begin(true)) {
    Serial.println("ERR: LittleFS mount failed");
    return;
  }
  if (!LittleFS.exists(kConfigPath)) {
    writeConfigFile();
    return;
  }
  String json = readFileToString(kConfigPath);
  String value;

  if (extractJsonValue(json, "baud", value)) {
    uint32_t baud = static_cast<uint32_t>(value.toInt());
    if (baud == 512 || baud == 1200 || baud == 2400) {
      config.baud = baud;
    }
  }
  if (extractJsonValue(json, "invert", value)) {
    parseBool(value, config.invert);
  }
  if (extractJsonValue(json, "idleHigh", value)) {
    parseBool(value, config.idleHigh);
  }
  if (extractJsonValue(json, "output", value)) {
    parseOutputMode(value, config.output);
  }
  if (extractJsonValue(json, "preambleMs", value)) {
    config.preambleMs = static_cast<uint32_t>(value.toInt());
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
    config.dataGpio = value.toInt();
  }
  if (extractJsonValue(json, "rfSenseGpio", value)) {
    config.rfSenseGpio = value.toInt();
  }
}

static void printStatus() {
  String outputMode = (config.output == OutputMode::kOpenDrain) ? "open_drain" : "push_pull";
  Serial.printf(
      "baud=%u invert=%s idleHigh=%s output=%s preambleMs=%u capInd=%u capGrp=%u "
      "functionBits=%u dataGpio=%d rfSenseGpio=%d\n",
      config.baud, config.invert ? "true" : "false", config.idleHigh ? "true" : "false",
      outputMode.c_str(), config.preambleMs, config.capInd, config.capGrp, config.functionBits,
      config.dataGpio, config.rfSenseGpio);
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

static void sendMessageOnce(const String &message, uint32_t capcode) {
  std::vector<uint8_t> bits =
      encoder.buildBitstream(capcode, config.functionBits, message, config.preambleMs, config.baud);
  if (!startTx(bits, config.baud, config.invert, config.idleHigh, config.output,
               config.dataGpio)) {
    Serial.println("BUSY");
    return;
  }
  waitForTxComplete();
}

static void handleScope(uint32_t durationMs) {
  std::vector<uint8_t> bits = buildAlternatingBits(durationMs, config.baud);
  if (!startTx(bits, config.baud, config.invert, config.idleHigh, config.output,
               config.dataGpio)) {
    Serial.println("BUSY");
    return;
  }
  waitForTxComplete();
}

static void runTestLoop(uint32_t seconds) {
  uint32_t start = millis();
  uint32_t next = start;
  while ((millis() - start) < (seconds * 1000UL)) {
    if (!isTxActive()) {
      sendMessageOnce("HELLO WORLD", config.capInd);
    }
    next += kDefaultIntervalMs;
    uint32_t now = millis();
    if (next > now) {
      delay(next - now);
    }
  }
}

static void printRfSense() {
  uint32_t count = rfSenseCount;
  uint32_t lastMs = rfSenseLastMs;
  uint64_t totalPeriod = rfSensePeriodTotal;
  uint32_t periodCount = rfSensePeriodCount;
  uint32_t lastSeenAgo = (lastMs == 0) ? 0 : (millis() - lastMs);
  float avgPeriod = periodCount == 0 ? 0.0f : static_cast<float>(totalPeriod) / periodCount;
  Serial.printf("count=%u lastSeenMsAgo=%u avgPeriodMs=%.2f\n", count, lastSeenAgo, avgPeriod);
}

static void applySetCommand(const String &key, const String &value) {
  String normalizedKey = key;
  normalizedKey.toLowerCase();
  int oldRfSense = config.rfSenseGpio;

  if (normalizedKey == "baud") {
    uint32_t baud = static_cast<uint32_t>(value.toInt());
    if (baud == 512 || baud == 1200 || baud == 2400) {
      config.baud = baud;
    }
  } else if (normalizedKey == "invert") {
    parseBool(value, config.invert);
  } else if (normalizedKey == "idlehigh") {
    parseBool(value, config.idleHigh);
  } else if (normalizedKey == "output") {
    parseOutputMode(value, config.output);
  } else if (normalizedKey == "preamblems") {
    config.preambleMs = static_cast<uint32_t>(value.toInt());
  } else if (normalizedKey == "capind") {
    config.capInd = static_cast<uint32_t>(value.toInt());
  } else if (normalizedKey == "capgrp") {
    config.capGrp = static_cast<uint32_t>(value.toInt());
  } else if (normalizedKey == "functionbits") {
    config.functionBits = static_cast<uint8_t>(value.toInt() & 0x3);
  } else if (normalizedKey == "datagpio") {
    config.dataGpio = value.toInt();
  } else if (normalizedKey == "rfsensegpio") {
    config.rfSenseGpio = value.toInt();
  } else {
    Serial.println("ERR: unknown key");
    return;
  }

  detachRfSenseIfNeeded(oldRfSense);
  attachRfSense();
  applyConfigPins();
  Serial.println("OK");
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

  if (cmd == "STATUS") {
    printStatus();
    return;
  }
  if (cmd == "SAVE") {
    writeConfigFile();
    Serial.println("SAVED");
    return;
  }
  if (cmd == "LOAD") {
    loadConfig();
    applyConfigPins();
    attachRfSense();
    Serial.println("LOADED");
    return;
  }
  if (cmd == "H") {
    sendMessageOnce("H", config.capInd);
    return;
  }
  if (cmd == "RFSENSE") {
    printRfSense();
    return;
  }
  if (cmd == "SCOPE") {
    String value = (space < 0) ? "" : trimmed.substring(space + 1);
    handleScope(static_cast<uint32_t>(value.toInt()));
    return;
  }
  if (cmd == "T1") {
    String value = (space < 0) ? "" : trimmed.substring(space + 1);
    runTestLoop(static_cast<uint32_t>(value.toInt()));
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

void setup() {
  Serial.begin(115200);
  delay(100);
  loadConfig();
  applyConfigPins();
  attachRfSense();
  setIdleLine();
  Serial.println("POCSAG ready. Commands: STATUS, SET, SAVE, LOAD, H, T1, SCOPE, RFSENSE");
}

void loop() {
  static String lineBuffer;
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (lineBuffer.length() > 0) {
        handleCommand(lineBuffer);
        lineBuffer = "";
      }
    } else {
      lineBuffer += c;
    }
  }
  delay(2);
}
