#include <Arduino.h>
#include <LittleFS.h>

#include <vector>

#include "driver/rmt.h"

namespace {
constexpr const char *kConfigPath = "/config.json";
constexpr uint32_t kSyncWord = 0x7CD215D8;
constexpr uint32_t kIdleWord = 0x7A89C197;
constexpr uint32_t kMaxRmtDuration = 32767;
}

enum class OutputMode : uint8_t { kOpenDrain = 0, kPushPull = 1 };

struct Config {
  uint32_t baud = 512;
  uint32_t preambleBits = 576;
  uint32_t capInd = 1422890;
  uint32_t capGrp = 1422890;
  uint8_t functionBits = 2;
  int dataGpio = 4;
  OutputMode output = OutputMode::kOpenDrain;
  bool invertWords = true;
  bool driveOneLow = true;
  bool idleHigh = true;
  String bootPreset = "ADVISOR";
  bool firstBootShown = false;
};

static Config config;

static void printStatus();

class WaveTx {
 public:
  bool isBusy() const { return busy_; }

  bool transmitBits(const std::vector<uint8_t> &bits, uint32_t baud, int gpio,
                    OutputMode output, bool idleHigh, bool driveOneLow) {
    if (busy_) {
      return false;
    }
    if (bits.empty()) {
      setIdleLine(gpio, output, idleHigh);
      return true;
    }
    if (!ensureConfig(gpio, output, idleHigh)) {
      return false;
    }
    uint32_t bitPeriodUs = (1000000 + (baud / 2)) / baud;
    buildItems(bits, bitPeriodUs, driveOneLow);
    if (items_.empty()) {
      setIdleLine(gpio, output, idleHigh);
      return true;
    }
    busy_ = true;
    rmt_write_items(channel_, items_.data(), items_.size(), true);
    rmt_wait_tx_done(channel_, portMAX_DELAY);
    busy_ = false;
    setIdleLine(gpio, output, idleHigh);
    return true;
  }

 private:
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
    setIdleLine(gpio_, output_, idleHigh_);
    rmt_config_t config = {};
    config.rmt_mode = RMT_MODE_TX;
    config.channel = channel_;
    config.gpio_num = static_cast<gpio_num_t>(gpio_);
    config.mem_block_num = 1;
    config.clk_div = 80;
    config.tx_config.loop_en = false;
    config.tx_config.carrier_en = false;
    config.tx_config.idle_output_en = true;
    config.tx_config.idle_level = idleHigh_ ? RMT_IDLE_LEVEL_HIGH : RMT_IDLE_LEVEL_LOW;
    if (rmt_config(&config) != ESP_OK) {
      return false;
    }
    if (rmt_driver_install(channel_, 0, 0) != ESP_OK) {
      return false;
    }
    initialized_ = true;
    return true;
  }

  void buildItems(const std::vector<uint8_t> &bits, uint32_t bitPeriodUs, bool driveOneLow) {
    items_.clear();
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
        totalDuration -= chunk;
      }
      index += runLength;
    }
  }

  void setIdleLine(int gpio, OutputMode output, bool idleHigh) {
    if (output == OutputMode::kOpenDrain) {
      pinMode(gpio, OUTPUT_OPEN_DRAIN);
    } else {
      pinMode(gpio, OUTPUT);
    }
    digitalWrite(gpio, idleHigh ? HIGH : LOW);
  }

  rmt_channel_t channel_ = RMT_CHANNEL_0;
  bool initialized_ = false;
  bool busy_ = false;
  int gpio_ = -1;
  OutputMode output_ = OutputMode::kOpenDrain;
  bool idleHigh_ = true;
  std::vector<rmt_item32_t> items_;
};

class PocsagEncoder {
 public:
  std::vector<uint32_t> buildBatchWords(uint32_t capcode, uint8_t functionBits,
                                        const String &message) const {
    return buildSingleBatch(capcode, functionBits, message);
  }

 private:
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
static WaveTx waveTx;

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

static void applyConfigPins() {
  if (config.output == OutputMode::kOpenDrain) {
    pinMode(config.dataGpio, OUTPUT_OPEN_DRAIN);
  } else {
    pinMode(config.dataGpio, OUTPUT);
  }
  digitalWrite(config.dataGpio, config.idleHigh ? HIGH : LOW);
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
    config.invertWords = true;
    config.driveOneLow = true;
    config.idleHigh = true;
  } else if (normalized == "generic" || normalized == "lora_baseline") {
    config.baud = 512;
    config.preambleBits = 1024;
    config.capInd = 1422890;
    config.capGrp = 1422890;
    config.functionBits = 0;
    config.dataGpio = 4;
    config.output = OutputMode::kOpenDrain;
    config.invertWords = false;
    config.driveOneLow = false;
    config.idleHigh = true;
  } else {
    if (report) {
      Serial.println("ERR: preset must be ADVISOR|GENERIC");
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
  file.printf("  \"preambleBits\": %u,\n", config.preambleBits);
  file.printf("  \"capInd\": %u,\n", config.capInd);
  file.printf("  \"capGrp\": %u,\n", config.capGrp);
  file.printf("  \"functionBits\": %u,\n", config.functionBits);
  file.printf("  \"dataGpio\": %d,\n", config.dataGpio);
  file.printf("  \"output\": \"%s\",\n", outputMode.c_str());
  file.printf("  \"invertWords\": %s,\n", config.invertWords ? "true" : "false");
  file.printf("  \"driveOneLow\": %s,\n", config.driveOneLow ? "true" : "false");
  file.printf("  \"idleHigh\": %s,\n", config.idleHigh ? "true" : "false");
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

static void applyWordInversion(std::vector<uint32_t> &words, bool invertWords) {
  if (!invertWords) {
    return;
  }
  for (uint32_t &word : words) {
    word = ~word;
  }
}

static std::vector<uint8_t> buildPocsagBits(const String &message, uint32_t capcode,
                                            uint32_t preambleBits) {
  std::vector<uint32_t> words;
  words.reserve(17);
  std::vector<uint8_t> bits;
  if (preambleBits > 0) {
    for (uint32_t i = 0; i < preambleBits; ++i) {
      bits.push_back(static_cast<uint8_t>(i % 2 == 0));
    }
  }
  words = encoder.buildBatchWords(capcode, config.functionBits, message);
  applyWordInversion(words, config.invertWords);
  uint32_t syncWord = config.invertWords ? ~kSyncWord : kSyncWord;
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

static bool sendMessageOnce(const String &message, uint32_t capcode) {
  std::vector<uint8_t> bits = buildPocsagBits(message, capcode, config.preambleBits);
  if (!waveTx.transmitBits(bits, config.baud, config.dataGpio, config.output, config.idleHigh,
                           config.driveOneLow)) {
    Serial.println("TX_BUSY");
    return false;
  }
  return true;
}

static void handleScope(uint32_t durationMs) {
  std::vector<uint8_t> bits = buildAlternatingBits(durationMs, config.baud);
  if (!waveTx.transmitBits(bits, config.baud, config.dataGpio, config.output, config.idleHigh,
                           config.driveOneLow)) {
    Serial.println("TX_BUSY");
    return;
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

  applyConfigPins();
  Serial.println("OK");
}

static void printHelp() {
  Serial.println("POCSAG TX (RMT) Commands:");
  Serial.println("  STATUS | HELP | ? | PRESET <name> | SCOPE <ms> | H | SEND <text> | T1 <sec>");
  Serial.println("  SET <key> <value> | SAVE | LOAD");
  Serial.println("Presets: ADVISOR | GENERIC");
  Serial.println("SET keys: baud preambleBits capInd capGrp functionBits dataGpio output");
  Serial.println("          invertWords driveOneLow idleHigh");
  Serial.println("Examples:");
  Serial.println("  PRESET ADVISOR");
  Serial.println("  STATUS");
  Serial.println("  SCOPE 2000");
  Serial.println("  H");
  Serial.println("  SEND HELLO WORLD");
  Serial.println("  T1 10");
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

void setup() {
  Serial.begin(115200);
  delay(100);
  loadConfig();
  if (!applyPreset(config.bootPreset, false)) {
    applyConfigPins();
  }
  applyConfigPins();
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
