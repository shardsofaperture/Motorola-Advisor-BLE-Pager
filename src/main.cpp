#include <Arduino.h>
#include <LittleFS.h>

#include <cmath>
#include <vector>

namespace {
constexpr const char *kConfigPath = "/config.json";
constexpr uint32_t kSyncWord = 0x7CD215D8;
constexpr uint32_t kIdleWord = 0x7A89C197;
constexpr uint32_t kMinFrameGapMs = 0;
constexpr uint32_t kMaxFrameGapMs = 1000;
}

enum class OutputMode : uint8_t { kOpenDrain = 0, kPushPull = 1 };
enum class AlphaBitOrder : uint8_t { kLsbFirst = 0, kMsbFirst = 1 };

struct Config {
  uint32_t baud = 512;
  bool invert = false;
  bool idleHigh = true;
  OutputMode output = OutputMode::kOpenDrain;
  uint32_t preambleMs = 2000;
  bool odPullup = false;
  String bootPreset = "pager";
  uint32_t capInd = 1422890;
  uint32_t capGrp = 1422890;
  uint8_t functionBits = 0;
  int dataGpio = 4;
  float baudScale = 1.0f;
  uint32_t frameGapMs = 60;
  bool repeatPreambleEachFrame = true;
  AlphaBitOrder alphaBitOrder = AlphaBitOrder::kLsbFirst;
  bool firstBootShown = false;
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
static bool txOdPullup = false;
static int txGpio = 4;

class PocsagEncoder {
 public:
  std::vector<uint8_t> buildBitstream(uint32_t capcode, uint8_t functionBits,
                                      AlphaBitOrder alphaBitOrder, const String &message,
                                      uint32_t preambleMs, uint32_t baud) const {
    std::vector<uint8_t> bits;
    appendPreamble(bits, preambleMs, baud);
    appendWord(bits, kSyncWord);
    std::vector<uint32_t> batch = buildSingleBatch(capcode, functionBits, alphaBitOrder, message);
    for (uint32_t word : batch) {
      appendWord(bits, word);
    }
    return bits;
  }

  std::vector<uint8_t> buildAddressOnlyBitstream(uint32_t capcode, uint8_t functionBits,
                                                 uint32_t preambleMs, uint32_t baud) const {
    std::vector<uint8_t> bits;
    appendPreamble(bits, preambleMs, baud);
    appendWord(bits, kSyncWord);
    std::vector<uint32_t> batch = buildAddressOnlyBatch(capcode, functionBits);
    for (uint32_t word : batch) {
      appendWord(bits, word);
    }
    return bits;
  }

  std::vector<uint32_t> buildBatchWords(uint32_t capcode, uint8_t functionBits,
                                        AlphaBitOrder alphaBitOrder, const String &message) const {
    return buildSingleBatch(capcode, functionBits, alphaBitOrder, message);
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
                                         AlphaBitOrder alphaBitOrder,
                                         const String &message) const {
    std::vector<uint32_t> words(16, kIdleWord);
    uint8_t frame = static_cast<uint8_t>(capcode & 0x7);
    uint32_t addressWord = buildAddressWord(capcode, functionBits);
    std::vector<uint32_t> messageWords = buildAlphaWords(alphaBitOrder, message);

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

  std::vector<uint32_t> buildAddressOnlyBatch(uint32_t capcode, uint8_t functionBits) const {
    std::vector<uint32_t> words(16, kIdleWord);
    uint8_t frame = static_cast<uint8_t>(capcode & 0x7);
    uint32_t addressWord = buildAddressWord(capcode, functionBits);

    size_t index = frame * 2;
    if (index < words.size()) {
      words[index] = addressWord;
    }
    return words;
  }

  std::vector<uint32_t> buildAlphaWords(AlphaBitOrder alphaBitOrder,
                                        const String &message) const {
    std::vector<uint8_t> bits;
    bits.reserve(message.length() * 7);
    for (size_t i = 0; i < message.length(); ++i) {
      uint8_t value = static_cast<uint8_t>(message[i]) & 0x7F;
      if (alphaBitOrder == AlphaBitOrder::kLsbFirst) {
        for (int b = 0; b < 7; ++b) {
          bits.push_back(static_cast<uint8_t>((value >> b) & 0x1));
        }
      } else {
        for (int b = 6; b >= 0; --b) {
          bits.push_back(static_cast<uint8_t>((value >> b) & 0x1));
        }
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
    constexpr uint32_t poly = 0x3B9;
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
static std::vector<uint8_t> txBits;

static void applyLineState(bool logicalHigh) {
  bool bit = txInvert ? !logicalHigh : logicalHigh;
  if (txOutput == OutputMode::kOpenDrain) {
    if (bit) {
      pinMode(txGpio, txOdPullup ? INPUT_PULLUP : INPUT);
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

static bool startTx(const std::vector<uint8_t> &bits, uint32_t baud, float baudScale,
                    bool invert, bool idleHigh, OutputMode outputMode, bool odPullup,
                    int dataGpio) {
  if (txActive) {
    setIdleLine();
    return false;
  }
  if (bits.empty()) {
    setIdleLine();
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
  txOdPullup = odPullup;
  txGpio = dataGpio;
  setIdleLine();
  float nominal = 1000000.0f / static_cast<float>(baud);
  uint32_t periodUs = static_cast<uint32_t>(lroundf(nominal * baudScale));
  if (periodUs < 1) {
    periodUs = 1;
  }
  timerAlarmWrite(txTimer, periodUs, true);
  txActive = true;
  timerAlarmEnable(txTimer);
  return true;
}

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

static bool parseAlphaBitOrder(const String &value, AlphaBitOrder &out) {
  String normalized = value;
  normalized.toLowerCase();
  if (normalized == "lsb_first") {
    out = AlphaBitOrder::kLsbFirst;
    return true;
  }
  if (normalized == "msb_first") {
    out = AlphaBitOrder::kMsbFirst;
    return true;
  }
  return false;
}

static void applyConfigPins() {
  txInvert = config.invert;
  txIdleHigh = config.idleHigh;
  txOutput = config.output;
  txOdPullup = config.odPullup;
  txGpio = config.dataGpio;
  setIdleLine();
}

static bool applyPreset(const String &name, bool report) {
  String normalized = name;
  normalized.toLowerCase();
  if (normalized == "pager") {
    config.baud = 512;
    config.invert = false;
    config.idleHigh = true;
    config.output = OutputMode::kOpenDrain;
    config.odPullup = false;
    config.preambleMs = 2000;
    config.capInd = 1422890;
    config.capGrp = 1422890;
    config.functionBits = 0;
    config.alphaBitOrder = AlphaBitOrder::kLsbFirst;
    config.baudScale = 1.0f;
    config.frameGapMs = 60;
    config.repeatPreambleEachFrame = true;
  } else if (normalized == "lora_baseline") {
    config.baud = 512;
    config.invert = true;
    config.idleHigh = true;
    config.output = OutputMode::kPushPull;
    config.odPullup = false;
    config.preambleMs = 2000;
    config.functionBits = 2;
    config.alphaBitOrder = AlphaBitOrder::kLsbFirst;
    config.baudScale = 0.991f;
    config.frameGapMs = 60;
    config.repeatPreambleEachFrame = true;
  } else if (normalized == "fast1200") {
    config.baud = 1200;
    config.invert = false;
    config.idleHigh = true;
    config.output = OutputMode::kOpenDrain;
    config.odPullup = false;
    config.preambleMs = 2000;
    config.capInd = 1422890;
    config.capGrp = 1422890;
    config.functionBits = 0;
    config.alphaBitOrder = AlphaBitOrder::kLsbFirst;
    config.baudScale = 1.0f;
    config.frameGapMs = 60;
    config.repeatPreambleEachFrame = true;
  } else {
    if (report) {
      Serial.println("ERR: preset must be pager|lora_baseline|fast1200");
    }
    return false;
  }
  applyConfigPins();
  if (report) {
    Serial.printf("OK PRESET %s\n", normalized.c_str());
    printStatus();
  }
  return true;
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
  file.printf("  \"odPullup\": %s,\n", config.odPullup ? "true" : "false");
  file.printf("  \"bootPreset\": \"%s\",\n", config.bootPreset.c_str());
  file.printf("  \"capInd\": %u,\n", config.capInd);
  file.printf("  \"capGrp\": %u,\n", config.capGrp);
  file.printf("  \"functionBits\": %u,\n", config.functionBits);
  file.printf("  \"dataGpio\": %d,\n", config.dataGpio);
  file.printf("  \"baudScale\": %.3f,\n", config.baudScale);
  file.printf("  \"frameGapMs\": %u,\n", config.frameGapMs);
  file.printf("  \"repeatPreambleEachFrame\": %s,\n",
              config.repeatPreambleEachFrame ? "true" : "false");
  const char *alphaOrder =
      (config.alphaBitOrder == AlphaBitOrder::kLsbFirst) ? "lsb_first" : "msb_first";
  file.printf("  \"alphaBitOrder\": \"%s\",\n", alphaOrder);
  file.printf("  \"firstBootShown\": %s\n", config.firstBootShown ? "true" : "false");
  file.println("}");
  file.close();
}

static bool loadConfig() {
  config = Config();
  bool shouldShowHelp = false;
  if (!LittleFS.begin(true)) {
    Serial.println("ERR: LittleFS mount failed");
    return false;
  }
  if (!LittleFS.exists(kConfigPath)) {
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
  if (extractJsonValue(json, "odPullup", value)) {
    parseBool(value, config.odPullup);
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
  if (extractJsonValue(json, "baudScale", value)) {
    float scale = value.toFloat();
    if (scale < 0.90f) {
      scale = 0.90f;
    } else if (scale > 1.10f) {
      scale = 1.10f;
    }
    config.baudScale = scale;
  }
  if (extractJsonValue(json, "frameGapMs", value)) {
    uint32_t gap = static_cast<uint32_t>(value.toInt());
    if (gap < kMinFrameGapMs) {
      gap = kMinFrameGapMs;
    } else if (gap > kMaxFrameGapMs) {
      gap = kMaxFrameGapMs;
    }
    config.frameGapMs = gap;
  }
  if (extractJsonValue(json, "repeatPreambleEachFrame", value)) {
    parseBool(value, config.repeatPreambleEachFrame);
  }
  if (extractJsonValue(json, "alphaBitOrder", value)) {
    parseAlphaBitOrder(value, config.alphaBitOrder);
  }
  if (extractJsonValue(json, "firstBootShown", value)) {
    parseBool(value, config.firstBootShown);
  }
  shouldShowHelp = !config.firstBootShown;
  return shouldShowHelp;
}

static void printStatus() {
  String outputMode = (config.output == OutputMode::kOpenDrain) ? "open_drain" : "push_pull";
  const char *alphaOrder =
      (config.alphaBitOrder == AlphaBitOrder::kLsbFirst) ? "lsb_first" : "msb_first";
  Serial.printf(
      "baud=%u invert=%s idleHigh=%s output=%s preambleMs=%u odPullup=%s bootPreset=%s "
      "capInd=%u capGrp=%u functionBits=%u dataGpio=%d baudScale=%.3f alphaBitOrder=%s "
      "frameGapMs=%u repeatPreambleEachFrame=%s\n",
      config.baud, config.invert ? "true" : "false", config.idleHigh ? "true" : "false",
      outputMode.c_str(), config.preambleMs, config.odPullup ? "true" : "false",
      config.bootPreset.c_str(), config.capInd, config.capGrp, config.functionBits,
      config.dataGpio, config.baudScale, alphaOrder, config.frameGapMs,
      config.repeatPreambleEachFrame ? "true" : "false");
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
  std::vector<uint8_t> bits = encoder.buildBitstream(
      capcode, config.functionBits, config.alphaBitOrder, message, config.preambleMs,
      config.baud);
  if (!startTx(bits, config.baud, config.baudScale, config.invert, config.idleHigh,
               config.output, config.odPullup, config.dataGpio)) {
    Serial.println("BUSY");
    return;
  }
  waitForTxComplete();
}

static void handleScope(uint32_t durationMs) {
  std::vector<uint8_t> bits = buildAlternatingBits(durationMs, config.baud);
  if (!startTx(bits, config.baud, config.baudScale, config.invert, config.idleHigh,
               config.output, config.odPullup, config.dataGpio)) {
    Serial.println("BUSY");
    return;
  }
  waitForTxComplete();
}

static void waitForTxAndGap() {
  waitForTxComplete();
  setIdleLine();
  delay(config.frameGapMs);
}

static bool startTxWithRetry(const std::vector<uint8_t> &bits) {
  while (!startTx(bits, config.baud, config.baudScale, config.invert, config.idleHigh,
                  config.output, config.odPullup, config.dataGpio)) {
    delay(5);
  }
  return true;
}

static uint32_t clampFrameGap(uint32_t gap) {
  if (gap < kMinFrameGapMs) {
    return kMinFrameGapMs;
  }
  if (gap > kMaxFrameGapMs) {
    return kMaxFrameGapMs;
  }
  return gap;
}

static bool isDigits(const String &value) {
  if (value.length() == 0) {
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    if (!isDigit(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }
  return true;
}

static void runMessageLoop(uint32_t seconds, const String &message, uint32_t capcode) {
  uint32_t start = millis();
  bool first = true;
  while ((millis() - start) < (seconds * 1000UL)) {
    uint32_t preambleMs =
        (config.repeatPreambleEachFrame || first) ? config.preambleMs : 0;
    std::vector<uint8_t> bits = encoder.buildBitstream(
        capcode, config.functionBits, config.alphaBitOrder, message, preambleMs, config.baud);
    startTxWithRetry(bits);
    waitForTxAndGap();
    first = false;
  }
}

static void runAddressLoop(uint32_t seconds, uint32_t capInd, uint32_t capGrp,
                           const String &mode) {
  uint32_t start = millis();
  bool first = true;
  bool sendIndNext = true;
  while ((millis() - start) < (seconds * 1000UL)) {
    uint32_t preambleMs =
        (config.repeatPreambleEachFrame || first) ? config.preambleMs : 0;
    uint32_t target = capInd;
    if (mode == "GRP") {
      target = capGrp;
    } else if (mode == "BOTH") {
      target = sendIndNext ? capInd : capGrp;
      sendIndNext = !sendIndNext;
    }
    std::vector<uint8_t> bits =
        encoder.buildAddressOnlyBitstream(target, config.functionBits, preambleMs, config.baud);
    startTxWithRetry(bits);
    waitForTxAndGap();
    first = false;
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
    int dataGpio = value.toInt();
    if (dataGpio > 0) {
      config.dataGpio = dataGpio;
    } else {
      config.dataGpio = 4;
    }
  } else if (normalizedKey == "baudscale") {
    float scale = value.toFloat();
    if (scale < 0.90f) {
      scale = 0.90f;
    } else if (scale > 1.10f) {
      scale = 1.10f;
    }
    config.baudScale = scale;
  } else if (normalizedKey == "alphabitorder") {
    if (!parseAlphaBitOrder(value, config.alphaBitOrder)) {
      Serial.println("ERR: alphaBitOrder must be lsb_first or msb_first");
      return;
    }
  } else if (normalizedKey == "framegapms") {
    uint32_t gap = static_cast<uint32_t>(value.toInt());
    config.frameGapMs = clampFrameGap(gap);
  } else if (normalizedKey == "repeatpreambleeachframe") {
    parseBool(value, config.repeatPreambleEachFrame);
  } else {
    Serial.println("ERR: unknown key");
    return;
  }

  applyConfigPins();
  Serial.println("OK");
}

static void printHelp() {
  Serial.println("Commands:");
  Serial.println("  STATUS | HELP | ? | PRESET <name> | SCOPE <ms> | H | T1 <sec>");
  Serial.println("  SET <key> <value> | SAVE | LOAD | ADDR <sec> [IND|GRP|BOTH]");
  Serial.println("  DIAG <msg> [capcode]");
  Serial.println("Presets: pager | lora_baseline | fast1200");
  Serial.println("SET keys: baud invert idleHigh output preambleMs capInd capGrp functionBits dataGpio");
  Serial.println("          baudScale alphaBitOrder frameGapMs repeatPreambleEachFrame");
  Serial.println("Examples:");
  Serial.println("  STATUS");
  Serial.println("  PRESET pager");
  Serial.println("  PRESET lora_baseline");
  Serial.println("  SCOPE 2000");
  Serial.println("  H");
  Serial.println("  T1 10");
  Serial.println("  SET invert true");
  Serial.println("  SET functionBits 2");
  Serial.println("  SET baudScale 0.991");
  Serial.println("  SAVE");
  Serial.println("  LOAD");
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
  if (cmd == "T1") {
    String value = (space < 0) ? "" : trimmed.substring(space + 1);
    runMessageLoop(static_cast<uint32_t>(value.toInt()), "HELLO WORLD", config.capInd);
    return;
  }
  if (cmd == "ADDR") {
    int secondSpace = (space < 0) ? -1 : trimmed.indexOf(' ', space + 1);
    String secondsValue = (space < 0) ? "" : trimmed.substring(space + 1, secondSpace);
    String mode = (secondSpace < 0) ? "IND" : trimmed.substring(secondSpace + 1);
    mode.trim();
    mode.toUpperCase();
    if (mode.length() == 0) {
      mode = "IND";
    }
    if (mode != "IND" && mode != "GRP" && mode != "BOTH") {
      Serial.println("ERR: ADDR <sec> [IND|GRP|BOTH]");
      return;
    }
    runAddressLoop(static_cast<uint32_t>(secondsValue.toInt()), config.capInd, config.capGrp,
                   mode);
    return;
  }
  if (cmd == "PRESET") {
    String value = (space < 0) ? "" : trimmed.substring(space + 1);
    value.trim();
    applyPreset(value, true);
    return;
  }
  if (cmd == "DIAG") {
    String remainder = (space < 0) ? "" : trimmed.substring(space + 1);
    remainder.trim();
    String message = remainder;
    uint32_t capcode = config.capInd;
    if (remainder.length() == 0) {
      message = "H";
    } else {
      int lastSpace = remainder.lastIndexOf(' ');
      if (lastSpace > 0) {
        String maybeCap = remainder.substring(lastSpace + 1);
        maybeCap.trim();
        String maybeMsg = remainder.substring(0, lastSpace);
        maybeMsg.trim();
        if (isDigits(maybeCap)) {
          capcode = static_cast<uint32_t>(maybeCap.toInt());
          message = (maybeMsg.length() > 0) ? maybeMsg : "H";
        }
      } else if (isDigits(remainder)) {
        capcode = static_cast<uint32_t>(remainder.toInt());
        message = "H";
      }
    }
    String outputMode = (config.output == OutputMode::kOpenDrain) ? "open_drain" : "push_pull";
    Serial.printf(
        "baud=%u invert=%s idleHigh=%s output=%s dataGpio=%d preambleMs=%u capcode=%u functionBits=%u\n",
        config.baud, config.invert ? "true" : "false", config.idleHigh ? "true" : "false",
        outputMode.c_str(), config.dataGpio, config.preambleMs, capcode, config.functionBits);
    Serial.printf("SYNC: %08lX\n", static_cast<unsigned long>(kSyncWord));
    std::vector<uint32_t> batch =
        encoder.buildBatchWords(capcode, config.functionBits, config.alphaBitOrder, message);
    for (size_t i = 0; i < batch.size(); ++i) {
      Serial.printf("W%02u: %08lX\n", static_cast<unsigned int>(i),
                    static_cast<unsigned long>(batch[i]));
    }
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

void setup() {
  Serial.begin(115200);
  delay(100);
  loadConfig();
  if (!applyPreset(config.bootPreset, false)) {
    applyConfigPins();
  }
  setIdleLine();
  printStatus();
  printHelp();
  if (!config.firstBootShown) {
    config.firstBootShown = true;
    writeConfigFile();
  }
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
