#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <functional>
#include <vector>

namespace {
constexpr const char *kDeviceName = "PagerBridge";

// Compile-time configuration.
constexpr int kDataGpio = 3; // D2 on XIAO ESP32-S3status
constexpr int kAlertGpio = -1;
constexpr uint32_t kDefaultCapcodeInd = 123456;
constexpr uint32_t kDefaultCapcodeGrp = 123457;
constexpr uint32_t kDefaultBaud = 512;
constexpr bool kDefaultInvert = false;
constexpr bool kDefaultIdleLineHigh = true;
constexpr uint8_t kDefaultFunctionBits = 0;
constexpr uint32_t kPocsagBatchWordCount = 17;
constexpr size_t kPageStoreCapacity = 20;
constexpr size_t kPersistPageCount = 3;
constexpr uint32_t kPreambleBits = 576;
constexpr uint32_t kDefaultPreambleMs = (kPreambleBits * 1000) / kDefaultBaud;
constexpr uint32_t kAlertWindowMs = 2000;
constexpr uint32_t kProbeGapMs = 500;

constexpr uint32_t kSyncWord = 0x7CD215D8;
constexpr uint32_t kIdleWord = 0x7A89C197;

static NimBLEServer *bleServer = nullptr;
static NimBLECharacteristic *rxCharacteristic = nullptr;
static NimBLECharacteristic *statusCharacteristic = nullptr;

static const NimBLEUUID kServiceUUID("1b0ee9b4-e833-5a9e-354c-7e2d486b2b7f");
static const NimBLEUUID kRxUUID("1b0ee9b4-e833-5a9e-354c-7e2d496b2b7f");
static const NimBLEUUID kStatusUUID("1b0ee9b4-e833-5a9e-354c-7e2d4a6b2b7f");

static Preferences preferences;

static std::deque<String> statusQueue;

struct PageRecord {
  uint32_t capcode = 0;
  String message;
  uint32_t timestampMs = 0;
  String lastResult;

  PageRecord() = default;
  PageRecord(uint32_t capcodeIn, const String &messageIn, uint32_t timestampMsIn,
             const String &resultIn)
      : capcode(capcodeIn),
        message(messageIn),
        timestampMs(timestampMsIn),
        lastResult(resultIn) {}
};

enum class OutputMode : uint8_t { kOpenDrain = 0, kPushPull = 1 };

constexpr OutputMode kDefaultOutputMode = OutputMode::kOpenDrain;

enum class InjectionProfile : uint8_t {
  kNrzSliced = 0,
  kNrzPushPull = 1,
  kManchester = 2,
  kDiscrimAudioFsk = 3,
  kSlicerEdgePulse = 4,
};

extern OutputMode configuredOutputMode;

bool send_min_page(InjectionProfile profile, uint32_t capcode, uint8_t functionBits,
                   uint32_t baud, bool invert, bool idleHigh, uint32_t preambleBits);
bool send_min_page(uint32_t capcode, uint8_t functionBits, uint32_t preambleMs);
bool send_min_page_with_settings(uint32_t capcode, uint8_t functionBits, uint32_t preambleMs,
                                 uint32_t baud, bool invert, bool idleHigh,
                                 OutputMode outputMode);

std::vector<int> parseGpioList(const String &value);

class PageStore {
 public:
  explicit PageStore(size_t capacity) : capacity_(capacity) {}

  void load(Preferences &prefs) {
    pages_.clear();
    const size_t stored = prefs.getUInt("pageCount", 0);
    for (size_t i = 0; i < stored && i < capacity_; ++i) {
      String key = String("page") + i;
      String value = prefs.getString(key.c_str(), "");
      if (value.length() == 0) {
        continue;
      }
      PageRecord record;
      if (deserialize(value, record)) {
        pages_.push_back(record);
      }
    }
  }

  void persist(Preferences &prefs) const {
    size_t stored = std::min(pages_.size(), static_cast<size_t>(kPersistPageCount));
    prefs.putUInt("pageCount", stored);
    for (size_t i = 0; i < stored; ++i) {
      size_t index = pages_.size() - stored + i;
      const PageRecord &record = pages_[index];
      String key = String("page") + i;
      prefs.putString(key.c_str(), serialize(record));
    }
  }

  void add(uint32_t capcode, const String &message, uint32_t timestampMs,
           const String &result) {
    if (pages_.size() == capacity_) {
      pages_.pop_front();
    }
    pages_.push_back(PageRecord{capcode, message, timestampMs, result});
  }

  void updateLatestResult(const String &result) {
    if (pages_.empty()) {
      return;
    }
    pages_.back().lastResult = result;
  }

  void clear() { pages_.clear(); }

  size_t size() const { return pages_.size(); }

  bool getByIndex(size_t indexFromNewest, PageRecord &out) const {
    if (indexFromNewest >= pages_.size()) {
      return false;
    }
    size_t index = pages_.size() - 1 - indexFromNewest;
    out = pages_[index];
    return true;
  }

  std::vector<String> listSummary() const {
    std::vector<String> lines;
    lines.reserve(pages_.size());
    size_t idx = 0;
    for (auto it = pages_.rbegin(); it != pages_.rend(); ++it, ++idx) {
      String line = String("#") + idx + " cap=" + it->capcode + " result=" + it->lastResult +
                    " ms=" + it->timestampMs + " msg=\"" + it->message + "\"";
      lines.push_back(line);
    }
    return lines;
  }

 private:
  String serialize(const PageRecord &record) const {
    String value = String(record.capcode) + "|" + record.timestampMs + "|" + record.lastResult +
                   "|" + record.message;
    return value;
  }

  bool deserialize(const String &value, PageRecord &record) const {
    int first = value.indexOf('|');
    int second = value.indexOf('|', first + 1);
    int third = value.indexOf('|', second + 1);
    if (first < 0 || second < 0) {
      return false;
    }
    record.capcode = value.substring(0, first).toInt();
    record.timestampMs = value.substring(first + 1, second).toInt();
    if (third < 0) {
      record.lastResult = "UNKNOWN";
      record.message = value.substring(second + 1);
    } else {
      record.lastResult = value.substring(second + 1, third);
      record.message = value.substring(third + 1);
    }
    return true;
  }

  size_t capacity_ = 0;
  std::deque<PageRecord> pages_;
};

class PocsagEncoder {
 public:
  std::vector<uint8_t> buildBitstream(uint32_t capcode, const String &message) const {
    std::vector<uint8_t> bits;
    bits.reserve(kPreambleBits + 2048);

    appendPreamble(bits);

    std::vector<uint32_t> codewords = buildCodewords(capcode, message, 0);
    for (uint32_t word : codewords) {
      appendWord(bits, word);
    }

    appendWord(bits, kIdleWord);
    appendWord(bits, kIdleWord);
    return bits;
  }

  std::vector<uint8_t> buildBitstreamFromCodewords(const std::vector<uint32_t> &codewords,
                                                   bool includePreamble) const {
    std::vector<uint8_t> bits;
    bits.reserve(kPreambleBits + codewords.size() * 32);
    if (includePreamble) {
      appendPreamble(bits);
    }
    for (uint32_t word : codewords) {
      appendWord(bits, word);
    }
    return bits;
  }

  std::vector<uint8_t> buildSingleBatch(uint32_t capcode, uint8_t functionBits,
                                        const String &message) const {
    std::vector<uint32_t> batch = buildSingleBatchCodewords(capcode, functionBits, &message);
    return buildBitstreamFromCodewords(batch, true);
  }

  std::vector<uint8_t> buildSingleBatchAddressOnly(uint32_t capcode, uint8_t functionBits) const {
    std::vector<uint32_t> batch = buildSingleBatchCodewords(capcode, functionBits, nullptr);
    return buildBitstreamFromCodewords(batch, true);
  }

  std::vector<uint32_t> buildSingleBatchCodewords(uint32_t capcode, uint8_t functionBits,
                                                  const String *message) const {
    std::vector<uint32_t> words;
    words.reserve(kPocsagBatchWordCount);
    words.push_back(kSyncWord);
    uint32_t ric = capcode / 2;
    uint8_t frame = ric % 8;
    uint32_t addressWord = buildAddressWord(capcode, functionBits);
    uint32_t messageWord = message ? buildMessageWordFromText(*message) : kIdleWord;
    for (uint8_t frameIndex = 0; frameIndex < 8; ++frameIndex) {
      if (frameIndex == frame) {
        words.push_back(addressWord);
        words.push_back(messageWord);
      } else {
        words.push_back(kIdleWord);
        words.push_back(kIdleWord);
      }
    }
    return words;
  }

  uint32_t buildAddressCodeword(uint32_t capcode, uint8_t functionBits) const {
    return buildAddressWord(capcode, functionBits);
  }

 private:
  void appendPreamble(std::vector<uint8_t> &bits) const {
    bits.reserve(bits.size() + kPreambleBits);
    for (uint32_t i = 0; i < kPreambleBits; ++i) {
      bits.push_back(static_cast<uint8_t>(i % 2 == 0));
    }
  }

  void appendWord(std::vector<uint8_t> &bits, uint32_t word) const {
    for (int i = 31; i >= 0; --i) {
      bits.push_back(static_cast<uint8_t>((word >> i) & 0x1));
    }
  }

  std::vector<uint32_t> buildCodewords(uint32_t capcode, const String &message,
                                       uint8_t functionBits) const {
    std::vector<uint32_t> codewords;
    std::vector<uint32_t> messageWords = buildMessageWords(message);
    uint32_t addressWord = buildAddressWord(capcode, functionBits);

    size_t messageIndex = 0;
    uint32_t ric = capcode / 2;
    uint8_t frame = ric % 8;
    bool addressInserted = false;

    bool done = false;
    while (!done) {
      codewords.push_back(kSyncWord);
      for (uint8_t frameIndex = 0; frameIndex < 8; ++frameIndex) {
        for (int slot = 0; slot < 2; ++slot) {
          if (!addressInserted && frameIndex == frame && slot == 0) {
            codewords.push_back(addressWord);
            addressInserted = true;
            continue;
          }
          if (!addressInserted && frameIndex < frame) {
            codewords.push_back(kIdleWord);
            continue;
          }
          if (messageIndex < messageWords.size()) {
            codewords.push_back(messageWords[messageIndex++]);
            continue;
          }
          codewords.push_back(kIdleWord);
        }
      }
      if (messageIndex >= messageWords.size()) {
        done = true;
      }
      if (!done) {
        frame = 0;
      }
    }
    if (codewords.size() == 1) {
      codewords.push_back(kIdleWord);
      codewords.push_back(kIdleWord);
    }
    return codewords;
  }

  std::vector<uint32_t> buildMessageWords(const String &message) const {
    return buildAlphaWords(message);
  }

  bool isNumericMessage(const String &message) const {
    for (size_t i = 0; i < message.length(); ++i) {
      char c = message[i];
      if (isdigit(static_cast<unsigned char>(c))) {
        continue;
      }
      if (c == ' ' || c == '-' || c == '(' || c == ')') {
        continue;
      }
      return false;
    }
    return true;
  }

  std::vector<uint32_t> buildNumericWords(const String &message) const {
    std::vector<uint8_t> nibbles;
    nibbles.reserve(message.length());
    for (size_t i = 0; i < message.length(); ++i) {
      nibbles.push_back(numericNibble(message[i]));
    }
    std::vector<uint32_t> words;
    size_t index = 0;
    while (index < nibbles.size()) {
      uint32_t data = 0;
      for (int i = 0; i < 5; ++i) {
        data <<= 4;
        if (index < nibbles.size()) {
          data |= (nibbles[index++] & 0xF);
        }
      }
      words.push_back(buildMessageWord(data));
    }
    if (words.empty()) {
      words.push_back(buildMessageWord(0));
    }
    return words;
  }

  uint8_t numericNibble(char c) const {
    if (c >= '0' && c <= '9') {
      return static_cast<uint8_t>(c - '0');
    }
    switch (c) {
      case ' ':
        return 0xA;
      case '-':
        return 0xB;
      case '(':
        return 0xC;
      case ')':
        return 0xD;
      default:
        return 0xA;
    }
  }

  std::vector<uint32_t> buildAlphaWords(const String &message) const {
    std::vector<uint8_t> bits;
    bits.reserve(message.length() * 7);
    for (size_t i = 0; i < message.length(); ++i) {
      uint8_t value = static_cast<uint8_t>(message[i]) & 0x7F;
      // POCSAG alphanumeric: append 7-bit ASCII LSB-first (bit 0 -> bit 6).
      // Example: "A" (0x41, b1000001) yields bit stream: 1 0 0 0 0 0 1,
      // which packs into the first payload as 0b1000001 followed by padding
      // zeros => data20 = 0x82000 (bits 19..13 set as 1 0 0 0 0 0 1).
      for (int b = 0; b <= 6; ++b) {
        bits.push_back(static_cast<uint8_t>((value >> b) & 0x1));
      }
    }
    std::vector<uint32_t> words;
    size_t index = 0;
    while (index < bits.size()) {
      uint32_t data = 0;
      for (int i = 0; i < 20; ++i) {
        // Pack bits in transmission order: first bit becomes MSB of payload.
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
    uint32_t ric = capcode / 2;
    uint32_t address = ric / 8;
    uint32_t data = (address & 0x3FFFF) << 2;
    data |= (functionBits & 0x3);
    uint32_t cw = buildCodeword(0, data);
    return cw;
  }

  uint32_t buildMessageWord(uint32_t data20) const {
    return buildCodeword(1, data20 & 0xFFFFF);
  }

  uint32_t buildMessageWordFromText(const String &message) const {
    std::vector<uint32_t> words = buildAlphaWords(message);
    if (!words.empty()) {
      return words.front();
    }
    return buildMessageWord(0);
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
    uint32_t temp = value;
    while (temp) {
      parity ^= (temp & 1u);
      temp >>= 1;
    }
    return parity & 0x1;
  }
};

class PocsagTx {
 public:
  PocsagTx() = default;

  void begin(int dataPin, bool invert, uint32_t baud, OutputMode outputMode, bool idleHigh) {
    instance_ = this;
    dataPin_ = dataPin;
    invert_ = invert;
    outputMode_ = outputMode;
    idleHigh_ = idleHigh;
    pinMode(dataPin_, INPUT);
    timer_ = timerBegin(0, 80, true);
    timerAttachInterrupt(timer_, &PocsagTx::onTimer, true);
    setBaud(baud);
    idleLine();
  }

  void setBaud(uint32_t baud) {
    baud_ = baud;
    if (timer_ != nullptr) {
      timerAlarmDisable(timer_);
      // 512 bps bit period: 1.953125 ms (1953.125 us).
      uint32_t periodUs = (1000000 + (baud_ / 2)) / baud_;
      timerAlarmWrite(timer_, periodUs, true);
    }
  }

  void setInvert(bool invert) {
    invert_ = invert;
    idleLine();
  }

  void setOutputMode(OutputMode outputMode) {
    outputMode_ = outputMode;
    idleLine();
  }

  void setIdleHigh(bool idleHigh) {
    idleHigh_ = idleHigh;
    idleLine();
  }

  void idle() { idleLine(); }

  bool isBusy() const { return sending_; }

  bool sendBits(std::vector<uint8_t> &&bits) {
    if (sending_ || bits.empty()) {
      return false;
    }
    bits_ = std::move(bits);
    bitIndex_ = 0;
    sending_ = true;
    timerAlarmWrite(timer_, bitPeriodUs(), true);
    timerAlarmEnable(timer_);
    return true;
  }

  bool consumeDoneFlag() {
    if (doneFlag_) {
      doneFlag_ = false;
      return true;
    }
    return false;
  }

 private:
  static void IRAM_ATTR onTimer() {
    instance_->handleTimer();
  }

  void IRAM_ATTR handleTimer() {
    if (!sending_) {
      return;
    }
    if (bitIndex_ >= bits_.size()) {
      timerAlarmDisable(timer_);
      sending_ = false;
      doneFlag_ = true;
      idleLine();
      return;
    }
    uint8_t bit = bits_[bitIndex_++];
    driveBit(bit);
  }

  void driveBit(uint8_t bit) {
    bool level = bit != 0;
    if (invert_) {
      level = !level;
    }
    applyLineLevel(level);
  }

  void idleLine() {
    bool idleLevel = idleHigh_;
    if (invert_) {
      idleLevel = !idleLevel;
    }
    applyLineLevel(idleLevel);
  }

  void applyLineLevel(bool level) {
    if (outputMode_ == OutputMode::kOpenDrain) {
      if (level) {
        pinMode(dataPin_, INPUT);
        return;
      }
      pinMode(dataPin_, OUTPUT);
      digitalWrite(dataPin_, LOW);
      return;
    }
    pinMode(dataPin_, OUTPUT);
    digitalWrite(dataPin_, level ? HIGH : LOW);
  }

  uint32_t bitPeriodUs() const { return (1000000 + (baud_ / 2)) / baud_; }

  static PocsagTx *instance_;
  int dataPin_ = -1;
  bool invert_ = false;
  OutputMode outputMode_ = OutputMode::kPushPull;
  bool idleHigh_ = true;
  uint32_t baud_ = 1200;
  hw_timer_t *timer_ = nullptr;
  volatile bool sending_ = false;
  volatile bool doneFlag_ = false;
  volatile size_t bitIndex_ = 0;
  std::vector<uint8_t> bits_;
};

PocsagTx *PocsagTx::instance_ = nullptr;

struct EncodedWaveform {
  std::vector<uint8_t> bits;
  uint32_t baud = 1200;
};

String injectionProfileName(InjectionProfile profile) {
  switch (profile) {
    case InjectionProfile::kNrzSliced:
      return "NRZ_SLICED";
    case InjectionProfile::kNrzPushPull:
      return "NRZ_PUSH_PULL";
    case InjectionProfile::kManchester:
      return "MANCHESTER";
    case InjectionProfile::kDiscrimAudioFsk:
      return "DISCRIM_AUDIO_FSK";
    case InjectionProfile::kSlicerEdgePulse:
      return "SLICER_EDGE_PULSE";
  }
  return "UNKNOWN";
}

void appendWordBits(std::vector<uint8_t> &bits, uint32_t word) {
  for (int i = 31; i >= 0; --i) {
    bits.push_back(static_cast<uint8_t>((word >> i) & 0x1));
  }
}

std::vector<uint8_t> buildMinimalNrzBits(const PocsagEncoder &encoder, uint32_t capcode,
                                         uint8_t functionBits, uint32_t preambleBits) {
  std::vector<uint8_t> bits;
  bits.reserve(preambleBits + (1 + 16) * 32);
  for (uint32_t i = 0; i < preambleBits; ++i) {
    bits.push_back(static_cast<uint8_t>((i % 2) == 0));
  }
  std::vector<uint32_t> batch = encoder.buildSingleBatchCodewords(capcode, functionBits, nullptr);
  for (uint32_t word : batch) {
    appendWordBits(bits, word);
  }
  return bits;
}

uint32_t preambleBitsFromMs(uint32_t baud, uint32_t preambleMs) {
  uint64_t bits =
      (static_cast<uint64_t>(baud) * static_cast<uint64_t>(preambleMs) + 999) / 1000;
  return std::max<uint32_t>(1, static_cast<uint32_t>(bits));
}

EncodedWaveform encodeManchester(const std::vector<uint8_t> &nrzBits, uint32_t baud) {
  EncodedWaveform waveform;
  waveform.bits.reserve(nrzBits.size() * 2);
  for (uint8_t bit : nrzBits) {
    if (bit) {
      waveform.bits.push_back(1);
      waveform.bits.push_back(0);
    } else {
      waveform.bits.push_back(0);
      waveform.bits.push_back(1);
    }
  }
  waveform.baud = baud * 2;
  return waveform;
}

EncodedWaveform encodeDiscrimAudioFsk(const std::vector<uint8_t> &nrzBits, uint32_t baud) {
  constexpr uint32_t kFskTickUs = 50;
  EncodedWaveform waveform;
  uint32_t bitPeriodUs = (1000000 + (baud / 2)) / baud;
  uint32_t ticksPerBit = std::max<uint32_t>(1, (bitPeriodUs + (kFskTickUs / 2)) / kFskTickUs);
  waveform.bits.reserve(nrzBits.size() * ticksPerBit);
  for (uint8_t bit : nrzBits) {
    uint32_t freq = bit ? 1200 : 2200;
    uint32_t halfPeriodUs = (1000000 + (freq)) / (freq * 2);
    uint32_t ticksPerHalf =
        std::max<uint32_t>(1, (halfPeriodUs + (kFskTickUs / 2)) / kFskTickUs);
    bool level = true;
    uint32_t tickCount = 0;
    for (uint32_t tick = 0; tick < ticksPerBit; ++tick) {
      waveform.bits.push_back(level ? 1 : 0);
      ++tickCount;
      if (tickCount >= ticksPerHalf) {
        tickCount = 0;
        level = !level;
      }
    }
  }
  waveform.baud = 1000000 / kFskTickUs;
  return waveform;
}

EncodedWaveform encodeSlicerEdgePulse(const std::vector<uint8_t> &nrzBits, uint32_t baud,
                                      bool idleHigh) {
  constexpr uint32_t kPulseUs = 100;
  constexpr uint32_t kPulseTickUs = 50;
  EncodedWaveform waveform;
  uint32_t bitPeriodUs = (1000000 + (baud / 2)) / baud;
  uint32_t ticksPerBit =
      std::max<uint32_t>(1, (bitPeriodUs + (kPulseTickUs / 2)) / kPulseTickUs);
  uint32_t pulseTicks =
      std::min<uint32_t>(ticksPerBit,
                         std::max<uint32_t>(1, (kPulseUs + (kPulseTickUs / 2)) / kPulseTickUs));
  waveform.bits.reserve(nrzBits.size() * ticksPerBit);
  uint8_t baseline = idleHigh ? 1 : 0;
  uint8_t pulseLevel = baseline ? 0 : 1;
  uint8_t previous = nrzBits.empty() ? 0 : nrzBits.front();
  for (uint8_t bit : nrzBits) {
    bool transition = bit != previous;
    if (transition) {
      for (uint32_t i = 0; i < pulseTicks; ++i) {
        waveform.bits.push_back(pulseLevel);
      }
      for (uint32_t i = pulseTicks; i < ticksPerBit; ++i) {
        waveform.bits.push_back(baseline);
      }
    } else {
      for (uint32_t i = 0; i < ticksPerBit; ++i) {
        waveform.bits.push_back(baseline);
      }
    }
    previous = bit;
  }
  waveform.baud = 1000000 / kPulseTickUs;
  return waveform;
}

struct TxRequest {
  uint32_t capcode = 0;
  String message;
  bool store = true;
  bool isRaw = false;
  std::vector<uint8_t> rawBits;
  String label;

  TxRequest() = default;
  TxRequest(uint32_t capcodeIn, const String &messageIn, bool storeIn)
      : capcode(capcodeIn), message(messageIn), store(storeIn) {}
  TxRequest(std::vector<uint8_t> &&bitsIn, const String &labelIn)
      : isRaw(true), rawBits(std::move(bitsIn)), label(labelIn) {}
};

enum class ProbeMode { kNone, kSequential, kBinary, kOneshot };

class ProbeController {
 public:
  ProbeController(PocsagTx &tx, const PocsagEncoder &encoder)
      : tx_(tx), encoder_(encoder) {}

  void begin(int alertPin) {
    alertPin_ = alertPin;
    if (alertPin_ >= 0) {
      pinMode(alertPin_, INPUT);
    }
  }

  bool startSequential(uint32_t startCap, uint32_t endCap, uint32_t step) {
    if (alertPin_ < 0) {
      return false;
    }
    mode_ = ProbeMode::kSequential;
    startCap_ = startCap;
    endCap_ = endCap;
    step_ = std::max<uint32_t>(step, 1);
    currentCap_ = startCap_;
    sequence_.clear();
    preparing_ = true;
    waitingForAlert_ = false;
    alertHigh_ = false;
    nextAllowedMs_ = 0;
    return true;
  }

  bool startBinary(uint32_t startCap, uint32_t endCap) {
    if (alertPin_ < 0) {
      return false;
    }
    mode_ = ProbeMode::kBinary;
    startCap_ = startCap;
    endCap_ = endCap;
    buildBinarySequence();
    currentIndex_ = 0;
    preparing_ = true;
    waitingForAlert_ = false;
    alertHigh_ = false;
    nextAllowedMs_ = 0;
    return true;
  }

  void stop() {
    mode_ = ProbeMode::kNone;
    preparing_ = false;
    waitingForAlert_ = false;
    sequence_.clear();
  }

  void startOneshot(const std::vector<uint32_t> &caps) {
    mode_ = ProbeMode::kOneshot;
    sequence_ = caps;
    currentIndex_ = 0;
    preparing_ = true;
    waitingForAlert_ = false;
    alertHigh_ = false;
    nextAllowedMs_ = 0;
  }

  // Probe sends test pages and watches ALERT_GPIO for pager activity.
  void update(std::function<void(uint32_t)> onHit, std::function<void(uint32_t)> onSend) {
    if (mode_ == ProbeMode::kNone) {
      return;
    }
    if (!waitingForAlert_ && preparing_ && !tx_.isBusy()) {
      if (!readyForNext()) {
        return;
      }
      uint32_t cap = nextCap();
      if (cap == 0xFFFFFFFF) {
        stop();
        return;
      }
      activeCap_ = cap;
      auto bits = encoder_.buildBitstream(cap, "PROBE");
      if (tx_.sendBits(std::move(bits))) {
        onSend(cap);
        if (mode_ == ProbeMode::kOneshot) {
          preparing_ = true;
          nextAllowedMs_ = millis() + kProbeGapMs;
        } else {
          waitingForAlert_ = true;
          alertStartMs_ = millis();
        }
      }
      preparing_ = false;
      return;
    }

    if (waitingForAlert_) {
      if (alertDetected()) {
        stop();
        onHit(activeCap_);
        return;
      }
      if (millis() - alertStartMs_ > kAlertWindowMs) {
        waitingForAlert_ = false;
        preparing_ = true;
        nextAllowedMs_ = millis() + kProbeGapMs;
      }
    }
  }

  bool readyForNext() const { return millis() >= nextAllowedMs_; }

  bool isActive() const { return mode_ != ProbeMode::kNone; }

 private:
  void buildBinarySequence() {
    sequence_.clear();
    if (startCap_ > endCap_) {
      std::swap(startCap_, endCap_);
    }
    std::vector<std::pair<uint32_t, uint32_t>> stack;
    stack.push_back({startCap_, endCap_});
    while (!stack.empty()) {
      std::pair<uint32_t, uint32_t> range = stack.back();
      stack.pop_back();
      uint32_t start = range.first;
      uint32_t end = range.second;
      if (start > end) {
        continue;
      }
      uint32_t mid = start + (end - start) / 2;
      sequence_.push_back(mid);
      if (mid > start) {
        stack.push_back({start, mid - 1});
      }
      if (mid < end) {
        stack.push_back({mid + 1, end});
      }
    }
  }

  uint32_t nextCap() {
    if (mode_ == ProbeMode::kSequential) {
      if (currentCap_ > endCap_) {
        return 0xFFFFFFFF;
      }
      uint32_t cap = currentCap_;
      currentCap_ += step_;
      return cap;
    }
    if (mode_ == ProbeMode::kBinary) {
      if (currentIndex_ >= sequence_.size()) {
        return 0xFFFFFFFF;
      }
      return sequence_[currentIndex_++];
    }
    if (mode_ == ProbeMode::kOneshot) {
      if (currentIndex_ >= sequence_.size()) {
        return 0xFFFFFFFF;
      }
      return sequence_[currentIndex_++];
    }
    return 0xFFFFFFFF;
  }

  bool alertDetected() {
    if (alertPin_ < 0) {
      return false;
    }
    int value = digitalRead(alertPin_);
    if (value == HIGH) {
      if (!alertHigh_) {
        alertHigh_ = true;
        alertHighStartMs_ = millis();
      }
      if (millis() - alertHighStartMs_ > 50) {
        return true;
      }
    } else {
      alertHigh_ = false;
    }
    return false;
  }

  PocsagTx &tx_;
  const PocsagEncoder &encoder_;
  int alertPin_ = -1;
  ProbeMode mode_ = ProbeMode::kNone;
  uint32_t startCap_ = 0;
  uint32_t endCap_ = 0;
  uint32_t step_ = 1;
  uint32_t currentCap_ = 0;
  uint32_t activeCap_ = 0;
  std::vector<uint32_t> sequence_;
  size_t currentIndex_ = 0;
  bool preparing_ = false;
  bool waitingForAlert_ = false;
  bool alertHigh_ = false;
  uint32_t alertHighStartMs_ = 0;
  uint32_t alertStartMs_ = 0;
  uint32_t nextAllowedMs_ = 0;
};

void queueStatus(const String &message);

class AutotestController {
 public:
  AutotestController(PocsagTx &tx, const PocsagEncoder &encoder)
      : tx_(tx), encoder_(encoder) {}

  void start(uint32_t capcode, uint32_t durationSeconds, uint32_t savedBaud,
             bool savedInvert, bool savedIdleHigh) {
    capcode_ = capcode;
    endTimeMs_ = millis() + durationSeconds * 1000;
    attempt_ = 0;
    baudIndex_ = 0;
    invertIndex_ = 0;
    idleIndex_ = 0;
    functionIndex_ = 0;
    preambleIndex_ = 0;
    stopRequested_ = false;
    active_ = true;
    savedBaud_ = savedBaud;
    savedInvert_ = savedInvert;
    savedIdleHigh_ = savedIdleHigh;
  }

  void requestStop() { stopRequested_ = true; }

  bool isActive() const { return active_; }

  void update() {
    if (!active_) {
      return;
    }
    if (tx_.isBusy()) {
      return;
    }
    if (stopRequested_ || millis() >= endTimeMs_) {
      finish(stopRequested_ ? "STATUS AUTOTEST STOPPED" : "STATUS AUTOTEST DONE");
      return;
    }
    AutotestSettings settings = currentSettings();
    ++attempt_;
    queueStatus(buildAttemptLine(settings));
    tx_.setBaud(settings.baud);
    tx_.setInvert(settings.invert);
    tx_.setIdleHigh(settings.idleHigh);
    auto bits = buildAutotestBits(settings);
    if (!tx_.sendBits(std::move(bits))) {
      queueStatus("ERROR AUTOTEST TX_BUSY");
      return;
    }
    advance();
  }

 private:
  struct AutotestSettings {
    uint32_t baud = 512;
    bool invert = false;
    bool idleHigh = true;
    uint8_t functionBits = 0;
    uint32_t preambleBits = 576;
  };

  static constexpr uint32_t kBauds[] = {512, 1200, 2400};
  static constexpr bool kInverts[] = {false, true};
  static constexpr bool kIdleHighs[] = {true, false};
  static constexpr uint32_t kPreambles[] = {576, 1152, 2304};
  static constexpr size_t kBaudCount = sizeof(kBauds) / sizeof(kBauds[0]);
  static constexpr size_t kInvertCount = sizeof(kInverts) / sizeof(kInverts[0]);
  static constexpr size_t kIdleHighCount = sizeof(kIdleHighs) / sizeof(kIdleHighs[0]);
  static constexpr size_t kPreambleCount = sizeof(kPreambles) / sizeof(kPreambles[0]);

  AutotestSettings currentSettings() const {
    AutotestSettings settings;
    settings.baud = kBauds[baudIndex_];
    settings.invert = kInverts[invertIndex_];
    settings.idleHigh = kIdleHighs[idleIndex_];
    settings.functionBits = static_cast<uint8_t>(functionIndex_);
    settings.preambleBits = kPreambles[preambleIndex_];
    return settings;
  }

  String buildAttemptLine(const AutotestSettings &settings) const {
    String line = "AUTOTEST #" + String(attempt_) + " CAP=" + String(capcode_) +
                  " BAUD=" + String(settings.baud) +
                  " INVERT=" + String(settings.invert ? 1 : 0) +
                  " IDLE=" + String(settings.idleHigh ? 1 : 0) +
                  " FUNC=" + String(settings.functionBits) +
                  " PREAMBLE=" + String(settings.preambleBits);
    return line;
  }

  void advance() {
    ++preambleIndex_;
    if (preambleIndex_ >= kPreambleCount) {
      preambleIndex_ = 0;
      ++functionIndex_;
      if (functionIndex_ >= 4) {
        functionIndex_ = 0;
        ++idleIndex_;
        if (idleIndex_ >= kIdleHighCount) {
          idleIndex_ = 0;
          ++invertIndex_;
          if (invertIndex_ >= kInvertCount) {
            invertIndex_ = 0;
            ++baudIndex_;
            if (baudIndex_ >= kBaudCount) {
              baudIndex_ = 0;
            }
          }
        }
      }
    }
  }

  std::vector<uint8_t> buildAutotestBits(const AutotestSettings &settings) const {
    std::vector<uint8_t> bits;
    bits.reserve(settings.preambleBits + (1 + 16) * 32);
    for (uint32_t i = 0; i < settings.preambleBits; ++i) {
      bits.push_back(static_cast<uint8_t>((i % 2) == 0));
    }
    appendWordBits(bits, kSyncWord);
    uint32_t ric = capcode_ / 2;
    uint8_t frame = ric % 8;
    uint32_t addressWord = encoder_.buildAddressCodeword(capcode_, settings.functionBits);
    for (uint8_t frameIndex = 0; frameIndex < 8; ++frameIndex) {
      if (frameIndex == frame) {
        appendWordBits(bits, addressWord);
        appendWordBits(bits, kIdleWord);
      } else {
        appendWordBits(bits, kIdleWord);
        appendWordBits(bits, kIdleWord);
      }
    }
    return bits;
  }

  void finish(const String &status) {
    active_ = false;
    stopRequested_ = false;
    tx_.setBaud(savedBaud_);
    tx_.setInvert(savedInvert_);
    tx_.setIdleHigh(savedIdleHigh_);
    queueStatus(status);
  }

  PocsagTx &tx_;
  const PocsagEncoder &encoder_;
  bool active_ = false;
  bool stopRequested_ = false;
  uint32_t capcode_ = 0;
  uint32_t endTimeMs_ = 0;
  size_t attempt_ = 0;
  size_t baudIndex_ = 0;
  size_t invertIndex_ = 0;
  size_t idleIndex_ = 0;
  size_t functionIndex_ = 0;
  size_t preambleIndex_ = 0;
  uint32_t savedBaud_ = kDefaultBaud;
  bool savedInvert_ = kDefaultInvert;
  bool savedIdleHigh_ = kDefaultIdleLineHigh;
};

constexpr uint32_t AutotestController::kBauds[];
constexpr bool AutotestController::kInverts[];
constexpr bool AutotestController::kIdleHighs[];
constexpr uint32_t AutotestController::kPreambles[];

class AutotestFastController {
 public:
  explicit AutotestFastController(PocsagTx &tx) : tx_(tx) {}

  void start(uint32_t durationSeconds, uint32_t savedBaud, bool savedInvert, bool savedIdleHigh,
             OutputMode savedOutputMode) {
    capcode_ = kDefaultCapcodeInd;
    startTimeMs_ = millis();
    endTimeMs_ = startTimeMs_ + durationSeconds * 1000;
    comboStartMs_ = 0;
    comboEndMs_ = 0;
    invertIndex_ = 0;
    functionIndex_ = 0;
    preambleIndex_ = 0;
    outputIndex_ = 0;
    stopRequested_ = false;
    active_ = true;
    nextAllowedMs_ = 0;
    attempt_ = 0;
    savedBaud_ = savedBaud;
    savedInvert_ = savedInvert;
    savedIdleHigh_ = savedIdleHigh;
    savedOutputMode_ = savedOutputMode;
  }

  void requestStop() { stopRequested_ = true; }

  bool isActive() const { return active_; }

  void update() {
    if (!active_) {
      return;
    }
    if (tx_.isBusy()) {
      return;
    }
    if (stopRequested_ || millis() >= endTimeMs_) {
      finish(stopRequested_ ? "STATUS AUTOTEST_FAST STOPPED" : "STATUS AUTOTEST_FAST DONE");
      return;
    }
    if (millis() < nextAllowedMs_) {
      return;
    }
    if (!ensureComboWindow()) {
      finish("STATUS AUTOTEST_FAST DONE");
      return;
    }
    if (!send_min_page_with_settings(capcode_, currentSettings_.functionBits,
                                     currentSettings_.preambleMs, kFixedBaud,
                                     currentSettings_.invert, configuredIdleHigh,
                                     currentSettings_.outputMode)) {
      queueStatus("ERROR AUTOTEST_FAST TX_BUSY");
      return;
    }
    nextAllowedMs_ = millis() + kFastGapMs;
  }

 private:
  struct AutotestFastSettings {
    bool invert = false;
    uint8_t functionBits = 0;
    uint32_t preambleMs = 800;
    OutputMode outputMode = OutputMode::kOpenDrain;
  };

  static constexpr uint32_t kFixedBaud = 512;
  static constexpr bool kInverts[] = {false, true};
  static constexpr uint32_t kPreamblesMs[] = {800, 1500, 2500};
  static constexpr OutputMode kOutputModes[] = {OutputMode::kOpenDrain,
                                                OutputMode::kPushPull};
  static constexpr size_t kInvertCount = sizeof(kInverts) / sizeof(kInverts[0]);
  static constexpr size_t kPreambleCount = sizeof(kPreamblesMs) / sizeof(kPreamblesMs[0]);
  static constexpr size_t kOutputCount = sizeof(kOutputModes) / sizeof(kOutputModes[0]);
  static constexpr uint32_t kFastGapMs = 200;

  AutotestFastSettings currentSettings() const {
    AutotestFastSettings settings;
    settings.invert = kInverts[invertIndex_];
    settings.functionBits = static_cast<uint8_t>(functionIndex_);
    settings.preambleMs = kPreamblesMs[preambleIndex_];
    settings.outputMode = kOutputModes[outputIndex_];
    return settings;
  }

  String buildAttemptLine(const AutotestFastSettings &settings) const {
    String line = "FAST invert=" + String(settings.invert ? 1 : 0) +
                  " func=" + String(settings.functionBits) +
                  " preamble=" + String(settings.preambleMs) + "ms output=" +
                  String(settings.outputMode == OutputMode::kOpenDrain ? "OPEN_DRAIN"
                                                                       : "PUSH_PULL");
    return line;
  }

  uint32_t comboDurationMs() const {
    uint32_t totalCombos =
        static_cast<uint32_t>(kInvertCount * kPreambleCount * kOutputCount * 4);
    uint32_t totalDurationMs = static_cast<uint32_t>(endTimeMs_ - startTimeMs_);
    uint32_t perCombo = totalDurationMs / std::max<uint32_t>(1, totalCombos);
    return std::max<uint32_t>(1000, std::min<uint32_t>(2000, perCombo));
  }

  bool ensureComboWindow() {
    uint32_t now = millis();
    if (comboStartMs_ == 0 || now >= comboEndMs_) {
      currentSettings_ = currentSettings();
      ++attempt_;
      queueStatus(buildAttemptLine(currentSettings_));
      comboStartMs_ = now;
      comboEndMs_ = now + comboDurationMs();
      advance();
    }
    return true;
  }

  void advance() {
    ++preambleIndex_;
    if (preambleIndex_ >= kPreambleCount) {
      preambleIndex_ = 0;
      ++functionIndex_;
      if (functionIndex_ >= 4) {
        functionIndex_ = 0;
        ++invertIndex_;
        if (invertIndex_ >= kInvertCount) {
          invertIndex_ = 0;
          ++outputIndex_;
          if (outputIndex_ >= kOutputCount) {
            outputIndex_ = 0;
          }
        }
      }
    }
  }

  void finish(const String &status) {
    active_ = false;
    stopRequested_ = false;
    tx_.setBaud(savedBaud_);
    tx_.setInvert(savedInvert_);
    tx_.setIdleHigh(savedIdleHigh_);
    tx_.setOutputMode(savedOutputMode_);
    queueStatus(status);
  }

  PocsagTx &tx_;
  bool active_ = false;
  bool stopRequested_ = false;
  uint32_t capcode_ = 0;
  uint32_t startTimeMs_ = 0;
  uint32_t endTimeMs_ = 0;
  uint32_t comboStartMs_ = 0;
  uint32_t comboEndMs_ = 0;
  uint32_t nextAllowedMs_ = 0;
  size_t attempt_ = 0;
  size_t invertIndex_ = 0;
  size_t functionIndex_ = 0;
  size_t preambleIndex_ = 0;
  size_t outputIndex_ = 0;
  AutotestFastSettings currentSettings_;
  uint32_t savedBaud_ = kDefaultBaud;
  bool savedInvert_ = kDefaultInvert;
  bool savedIdleHigh_ = kDefaultIdleLineHigh;
  OutputMode savedOutputMode_ = kDefaultOutputMode;
};

constexpr bool AutotestFastController::kInverts[];
constexpr uint32_t AutotestFastController::kPreamblesMs[];
constexpr OutputMode AutotestFastController::kOutputModes[];

class MinLoopController {
 public:
  explicit MinLoopController(PocsagTx &tx) : tx_(tx) {}

  void start(uint32_t capcode, uint8_t functionBits, uint32_t preambleMs,
             uint32_t durationSeconds) {
    capcode_ = capcode;
    functionBits_ = functionBits;
    preambleMs_ = preambleMs;
    endTimeMs_ = millis() + durationSeconds * 1000;
    nextAllowedMs_ = 0;
    active_ = true;
    stopRequested_ = false;
  }

  void requestStop() { stopRequested_ = true; }

  bool isActive() const { return active_; }

  void update() {
    if (!active_) {
      return;
    }
    if (stopRequested_ || millis() >= endTimeMs_) {
      finish(stopRequested_ ? "STATUS SEND_MIN_LOOP STOPPED" : "STATUS SEND_MIN_LOOP DONE");
      return;
    }
    if (tx_.isBusy()) {
      return;
    }
    if (millis() < nextAllowedMs_) {
      return;
    }
    if (!send_min_page(capcode_, functionBits_, preambleMs_)) {
      queueStatus("ERROR SEND_MIN_LOOP TX_BUSY");
      return;
    }
    nextAllowedMs_ = millis() + kLoopGapMs;
  }

 private:
  static constexpr uint32_t kLoopGapMs = 200;

  void finish(const String &status) {
    active_ = false;
    stopRequested_ = false;
    queueStatus(status);
  }

  PocsagTx &tx_;
  bool active_ = false;
  bool stopRequested_ = false;
  uint32_t capcode_ = 0;
  uint8_t functionBits_ = 0;
  uint32_t preambleMs_ = 0;
  uint32_t endTimeMs_ = 0;
  uint32_t nextAllowedMs_ = 0;
};

class Autotest2Controller {
 public:
  explicit Autotest2Controller(PocsagTx &tx) : tx_(tx) {}

  void start(uint32_t capcode, uint32_t durationSeconds, uint32_t savedBaud,
             bool savedInvert, bool savedIdleHigh, OutputMode savedOutputMode,
             int savedGpio, const String &gpioList) {
    capcode_ = capcode;
    endTimeMs_ = millis() + durationSeconds * 1000;
    attempt_ = 0;
    baudIndex_ = 0;
    invertIndex_ = 0;
    idleIndex_ = 0;
    functionIndex_ = 0;
    preambleIndex_ = 0;
    profileIndex_ = 0;
    gpioIndex_ = 0;
    stopRequested_ = false;
    active_ = true;
    nextAllowedMs_ = 0;
    savedBaud_ = savedBaud;
    savedInvert_ = savedInvert;
    savedIdleHigh_ = savedIdleHigh;
    savedOutputMode_ = savedOutputMode;
    savedGpio_ = savedGpio;
    gpioList_ = parseGpioList(gpioList);
    if (gpioList_.empty()) {
      gpioList_.push_back(savedGpio_);
    }
    currentGpio_ = -1;
  }

  void requestStop() { stopRequested_ = true; }

  bool isActive() const { return active_; }

  void update() {
    if (!active_) {
      return;
    }
    if (tx_.isBusy()) {
      return;
    }
    if (stopRequested_ || millis() >= endTimeMs_) {
      finish(stopRequested_ ? "STATUS AUTOTEST2 STOPPED" : "STATUS AUTOTEST2 DONE");
      return;
    }
    if (millis() < nextAllowedMs_) {
      return;
    }
    AutotestSettings settings = currentSettings();
    ++attempt_;
    queueStatus(buildAttemptLine(settings));
    if (settings.gpio != currentGpio_) {
      currentGpio_ = settings.gpio;
      tx_.begin(currentGpio_, settings.invert, settings.baud, configuredOutputMode,
                settings.idleHigh);
    }
    if (!send_min_page(settings.profile, capcode_, settings.functionBits, settings.baud,
                       settings.invert, settings.idleHigh, settings.preambleBits)) {
      queueStatus("ERROR AUTOTEST2 TX_BUSY");
      return;
    }
    nextAllowedMs_ = millis() + kAutotestGapMs;
    advance();
  }

 private:
  struct AutotestSettings {
    InjectionProfile profile = InjectionProfile::kNrzSliced;
    uint32_t baud = 512;
    bool invert = false;
    bool idleHigh = true;
    uint8_t functionBits = 0;
    uint32_t preambleBits = 576;
    int gpio = -1;
  };

  static constexpr uint32_t kBauds[] = {512, 1200, 2400};
  static constexpr bool kInverts[] = {false, true};
  static constexpr bool kIdleHighs[] = {true, false};
  static constexpr uint32_t kPreambles[] = {576, 1152, 2304};
  static constexpr InjectionProfile kProfiles[] = {
      InjectionProfile::kNrzSliced, InjectionProfile::kNrzPushPull,
      InjectionProfile::kManchester, InjectionProfile::kDiscrimAudioFsk,
      InjectionProfile::kSlicerEdgePulse};
  static constexpr size_t kBaudCount = sizeof(kBauds) / sizeof(kBauds[0]);
  static constexpr size_t kInvertCount = sizeof(kInverts) / sizeof(kInverts[0]);
  static constexpr size_t kIdleHighCount = sizeof(kIdleHighs) / sizeof(kIdleHighs[0]);
  static constexpr size_t kPreambleCount = sizeof(kPreambles) / sizeof(kPreambles[0]);
  static constexpr size_t kProfileCount = sizeof(kProfiles) / sizeof(kProfiles[0]);
  static constexpr uint32_t kAutotestGapMs = 350;

  AutotestSettings currentSettings() const {
    AutotestSettings settings;
    settings.profile = kProfiles[profileIndex_];
    settings.baud = kBauds[baudIndex_];
    settings.invert = kInverts[invertIndex_];
    settings.idleHigh = kIdleHighs[idleIndex_];
    settings.functionBits = static_cast<uint8_t>(functionIndex_);
    settings.preambleBits = kPreambles[preambleIndex_];
    settings.gpio = gpioList_[gpioIndex_];
    return settings;
  }

  String buildAttemptLine(const AutotestSettings &settings) const {
    String line = "AUTOTEST2 #" + String(attempt_) + " GPIO=" + String(settings.gpio) +
                  " PROFILE=" + injectionProfileName(settings.profile) +
                  " BAUD=" + String(settings.baud) +
                  " INVERT=" + String(settings.invert ? 1 : 0) +
                  " IDLE=" + String(settings.idleHigh ? 1 : 0) +
                  " FUNC=" + String(settings.functionBits) +
                  " PREAMBLE=" + String(settings.preambleBits);
    return line;
  }

  void advance() {
    ++preambleIndex_;
    if (preambleIndex_ >= kPreambleCount) {
      preambleIndex_ = 0;
      ++functionIndex_;
      if (functionIndex_ >= 4) {
        functionIndex_ = 0;
        ++idleIndex_;
        if (idleIndex_ >= kIdleHighCount) {
          idleIndex_ = 0;
          ++invertIndex_;
          if (invertIndex_ >= kInvertCount) {
            invertIndex_ = 0;
            ++baudIndex_;
            if (baudIndex_ >= kBaudCount) {
              baudIndex_ = 0;
              ++profileIndex_;
              if (profileIndex_ >= kProfileCount) {
                profileIndex_ = 0;
                ++gpioIndex_;
                if (gpioIndex_ >= gpioList_.size()) {
                  gpioIndex_ = 0;
                }
              }
            }
          }
        }
      }
    }
  }

  void finish(const String &status) {
    active_ = false;
    stopRequested_ = false;
    tx_.begin(savedGpio_, savedInvert_, savedBaud_, savedOutputMode_, savedIdleHigh_);
    queueStatus(status);
  }

  PocsagTx &tx_;
  bool active_ = false;
  bool stopRequested_ = false;
  uint32_t capcode_ = 0;
  uint32_t endTimeMs_ = 0;
  size_t attempt_ = 0;
  size_t baudIndex_ = 0;
  size_t invertIndex_ = 0;
  size_t idleIndex_ = 0;
  size_t functionIndex_ = 0;
  size_t preambleIndex_ = 0;
  size_t profileIndex_ = 0;
  size_t gpioIndex_ = 0;
  uint32_t nextAllowedMs_ = 0;
  uint32_t savedBaud_ = kDefaultBaud;
  bool savedInvert_ = kDefaultInvert;
  bool savedIdleHigh_ = kDefaultIdleLineHigh;
  OutputMode savedOutputMode_ = kDefaultOutputMode;
  int savedGpio_ = kDataGpio;
  int currentGpio_ = -1;
  std::vector<int> gpioList_;
};

constexpr uint32_t Autotest2Controller::kBauds[];
constexpr bool Autotest2Controller::kInverts[];
constexpr bool Autotest2Controller::kIdleHighs[];
constexpr uint32_t Autotest2Controller::kPreambles[];
constexpr InjectionProfile Autotest2Controller::kProfiles[];

class CommandParser {
 public:
  using Handler = std::function<void(const std::vector<String> &)>;

  void setHandler(Handler handler) { handler_ = std::move(handler); }

  void handleInput(const String &input) {
    int start = 0;
    while (start < input.length()) {
      int end = input.indexOf('\n', start);
      if (end < 0) {
        end = input.length();
      }
      String line = input.substring(start, end);
      line.trim();
      if (line.length() > 0) {
        parseLine(line);
      }
      start = end + 1;
    }
  }

 private:
  void parseLine(const String &line) {
    if (line.startsWith("{")) {
      String cmdLine = parseJson(line);
      if (cmdLine.length() > 0) {
        parseLine(cmdLine);
      }
      return;
    }

    std::vector<String> tokens;
    splitTokens(line, tokens);
    if (!tokens.empty() && handler_) {
      handler_(tokens);
    }
  }

  String parseJson(const String &line) {
    String cmd = getJsonString(line, "cmd");
    if (cmd.length() == 0) {
      cmd = getJsonString(line, "command");
    }
    cmd.toUpperCase();
    if (cmd.length() == 0) {
      return "";
    }
    if (cmd == "PAGE") {
      String cap = getJsonString(line, "capcode");
      String text = getJsonString(line, "text");
      if (cap.length() > 0) {
        return cmd + " " + cap + " " + text;
      }
      return cmd + " " + text;
    }
    if (cmd == "SET") {
      String key = getJsonString(line, "key");
      String value = getJsonString(line, "value");
      if (key.length() > 0 && value.length() > 0) {
        return cmd + " " + key + " " + value;
      }
    }
    if (cmd == "PROBE") {
      String mode = getJsonString(line, "mode");
      mode.toUpperCase();
      if (mode.length() > 0) {
        String start = getJsonString(line, "startCap");
        String end = getJsonString(line, "endCap");
        String step = getJsonString(line, "step");
        if (mode == "START" && step.length() > 0) {
          return cmd + " START " + start + " " + end + " " + step;
        }
        if (mode == "BINARY") {
          return cmd + " BINARY " + start + " " + end;
        }
      }
    }
    return cmd;
  }

  String getJsonString(const String &line, const char *key) {
    String pattern = String("\"") + key + "\"";
    int keyIndex = line.indexOf(pattern);
    if (keyIndex < 0) {
      return "";
    }
    int colon = line.indexOf(':', keyIndex + pattern.length());
    if (colon < 0) {
      return "";
    }
    int start = colon + 1;
    while (start < line.length() && isspace(static_cast<unsigned char>(line[start]))) {
      ++start;
    }
    if (start >= line.length()) {
      return "";
    }
    if (line[start] == '"') {
      int end = line.indexOf('"', start + 1);
      if (end < 0) {
        return "";
      }
      return line.substring(start + 1, end);
    }
    int end = start;
    while (end < line.length() && line[end] != ',' && line[end] != '}' &&
           !isspace(static_cast<unsigned char>(line[end]))) {
      ++end;
    }
    return line.substring(start, end);
  }

  void splitTokens(const String &line, std::vector<String> &tokens) {
    bool inQuotes = false;
    String current;
    for (size_t i = 0; i < line.length(); ++i) {
      char c = line[i];
      if (c == '"') {
        inQuotes = !inQuotes;
        continue;
      }
      if (!inQuotes && isspace(static_cast<unsigned char>(c))) {
        if (current.length() > 0) {
          tokens.push_back(current);
          current.clear();
        }
        continue;
      }
      current += c;
    }
    if (current.length() > 0) {
      tokens.push_back(current);
    }
  }

  Handler handler_;
};

static PocsagEncoder encoder;
static PocsagTx tx;
static PageStore pageStore(kPageStoreCapacity);
static ProbeController probe(tx, encoder);
static AutotestController autotest(tx, encoder);
static AutotestFastController autotestFast(tx);
static Autotest2Controller autotest2(tx);
static MinLoopController minLoop(tx);
static CommandParser parser;

static std::deque<TxRequest> txQueue;
static uint32_t configuredCapcodeInd = kDefaultCapcodeInd;
static uint32_t configuredCapcodeGrp = kDefaultCapcodeGrp;
static uint32_t configuredBaud = kDefaultBaud;
static bool configuredInvert = kDefaultInvert;
OutputMode configuredOutputMode = OutputMode::kOpenDrain;
static int configuredDataGpio = kDataGpio;
static String configuredGpioList;
static bool configuredIdleHigh = kDefaultIdleLineHigh;
static bool configuredAutoProbe = false;
static bool pendingStoredPage = false;
static bool capGrpWasExplicitlySet = false;
static uint8_t configuredDefaultFunction = kDefaultFunctionBits;
static uint32_t configuredDefaultPreambleMs = kDefaultPreambleMs;
static bool pendingDebugScope = false;

static String serialBuffer;

void emitStatus(const String &message) {
  statusQueue.push_back(message);
  if (Serial) {
    Serial.println(message);
  }
}

void queueStatus(const String &message) {
  emitStatus(message);
}

void saveSettings() {
  preferences.putUInt("cap_ind", configuredCapcodeInd);
  preferences.putUInt("cap_grp", configuredCapcodeGrp);
  preferences.putUInt("capcode", configuredCapcodeInd);
  preferences.putUInt("baud", configuredBaud);
  preferences.putBool("invert", configuredInvert);
  preferences.putUChar("output", static_cast<uint8_t>(configuredOutputMode));
  preferences.putInt("dataGpio", configuredDataGpio);
  preferences.putString("gpioList", configuredGpioList);
  preferences.putBool("idleHigh", configuredIdleHigh);
  preferences.putBool("autoProbe", configuredAutoProbe);
  pageStore.persist(preferences);
}

void notifyStatus() {
  if (statusCharacteristic == nullptr) {
    return;
  }
  if (statusQueue.empty()) {
    return;
  }
  if (statusCharacteristic->getSubscribedCount() == 0) {
    statusQueue.clear();
    return;
  }
  String message = statusQueue.front();
  statusQueue.pop_front();
  statusCharacteristic->setValue(message.c_str());
  statusCharacteristic->notify();
}

void enqueuePage(uint32_t capcode, const String &message, bool store) {
  txQueue.push_back(TxRequest{capcode, message, store});
}

void enqueueCarrier(uint32_t durationMs) {
  uint64_t bitCount = (static_cast<uint64_t>(configuredBaud) * durationMs) / 1000;
  if (bitCount == 0) {
    bitCount = 1;
  }
  std::vector<uint8_t> bits;
  bits.reserve(static_cast<size_t>(bitCount));
  for (uint64_t i = 0; i < bitCount; ++i) {
    bits.push_back(static_cast<uint8_t>((i % 2) == 0));
  }
  txQueue.push_back(TxRequest{std::move(bits), "CARRIER"});
}

void enqueueRawBits(std::vector<uint8_t> &&bits, const String &label) {
  txQueue.push_back(TxRequest{std::move(bits), label});
}

std::vector<uint8_t> buildAlternatingBitsForBaud(uint32_t durationMs, uint32_t baud) {
  uint64_t bitCount = (static_cast<uint64_t>(baud) * durationMs) / 1000;
  if (bitCount == 0) {
    bitCount = 1;
  }
  std::vector<uint8_t> bits;
  bits.reserve(static_cast<size_t>(bitCount));
  for (uint64_t i = 0; i < bitCount; ++i) {
    bits.push_back(static_cast<uint8_t>((i % 2) == 0));
  }
  return bits;
}

std::vector<uint8_t> buildAlternatingBits(uint32_t durationMs) {
  return buildAlternatingBitsForBaud(durationMs, configuredBaud);
}

std::vector<uint8_t> buildTestPattern(uint32_t capcode, uint8_t functionBits,
                                      const String &message) {
  std::vector<uint8_t> bits = buildAlternatingBits(2000);
  std::vector<uint8_t> batchBits = encoder.buildSingleBatch(capcode, functionBits, message);
  bits.insert(bits.end(), batchBits.begin(), batchBits.end());
  return bits;
}

bool send_min_page(InjectionProfile profile, uint32_t capcode, uint8_t functionBits,
                   uint32_t baud, bool invert, bool idleHigh, uint32_t preambleBits) {
  std::vector<uint8_t> nrzBits =
      buildMinimalNrzBits(encoder, capcode, functionBits, preambleBits);
  EncodedWaveform waveform;
  switch (profile) {
    case InjectionProfile::kNrzSliced:
    case InjectionProfile::kNrzPushPull:
      waveform.bits = std::move(nrzBits);
      waveform.baud = baud;
      break;
    case InjectionProfile::kManchester:
      waveform = encodeManchester(nrzBits, baud);
      break;
    case InjectionProfile::kDiscrimAudioFsk:
      waveform = encodeDiscrimAudioFsk(nrzBits, baud);
      break;
    case InjectionProfile::kSlicerEdgePulse:
      waveform = encodeSlicerEdgePulse(nrzBits, baud, idleHigh);
      break;
  }
  if (waveform.bits.empty()) {
    return false;
  }
  OutputMode outputMode = configuredOutputMode;
  if (profile == InjectionProfile::kNrzPushPull) {
    outputMode = OutputMode::kPushPull;
  }
  tx.setOutputMode(outputMode);
  tx.setInvert(invert);
  tx.setIdleHigh(idleHigh);
  tx.setBaud(waveform.baud);
  return tx.sendBits(std::move(waveform.bits));
}

bool send_min_page_with_settings(uint32_t capcode, uint8_t functionBits, uint32_t preambleMs,
                                 uint32_t baud, bool invert, bool idleHigh,
                                 OutputMode outputMode) {
  uint32_t preambleBits = preambleBitsFromMs(baud, preambleMs);
  std::vector<uint8_t> nrzBits = buildMinimalNrzBits(encoder, capcode, functionBits, preambleBits);
  if (nrzBits.empty()) {
    return false;
  }
  tx.setOutputMode(outputMode);
  tx.setInvert(invert);
  tx.setIdleHigh(idleHigh);
  tx.setBaud(baud);
  return tx.sendBits(std::move(nrzBits));
}

bool send_min_page(uint32_t capcode, uint8_t functionBits, uint32_t preambleMs) {
  return send_min_page_with_settings(capcode, functionBits, preambleMs, configuredBaud,
                                     configuredInvert, configuredIdleHigh,
                                     configuredOutputMode);
}

String buildStatusLine() {
  String status = "STATUS CAPCODE=" + String(configuredCapcodeInd) +
                  " CAPIND=" + String(configuredCapcodeInd) +
                  " CAPGRP=" + String(configuredCapcodeGrp) +
                  " BAUD=" + String(configuredBaud) +
                  " INVERT=" + String(configuredInvert ? 1 : 0) +
                  " IDLE=" + String(configuredIdleHigh ? 1 : 0) +
                  " OUTPUT=" +
                  String(configuredOutputMode == OutputMode::kPushPull ? "PUSH_PULL"
                                                                       : "OPEN_DRAIN") +
                  " DATA_GPIO=" + String(configuredDataGpio) +
                  " FUNCTION=" + String(configuredDefaultFunction) +
                  " PREAMBLE_MS=" + String(configuredDefaultPreambleMs) +
                  (configuredGpioList.length() > 0
                       ? " GPIO_LIST=" + configuredGpioList
                       : "") +
                  " ALERT_GPIO=" + String(kAlertGpio) +
                  " AUTOPROBE=" + String(configuredAutoProbe ? 1 : 0) +
                  " PAGES=" + String(pageStore.size());
  return status;
}

bool parseUint32(const String &value, uint32_t &out) {
  if (value.length() == 0) {
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    if (!isdigit(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }
  out = value.toInt();
  return true;
}

bool parseFunctionBits(const String &value, uint8_t &out) {
  uint32_t parsed = 0;
  if (!parseUint32(value, parsed) || parsed > 3) {
    return false;
  }
  out = static_cast<uint8_t>(parsed);
  return true;
}

bool isValidRate(uint32_t rate) { return rate == 512 || rate == 1200 || rate == 2400; }

std::vector<int> parseGpioList(const String &value) {
  std::vector<int> pins;
  int start = 0;
  while (start < value.length()) {
    int comma = value.indexOf(',', start);
    if (comma < 0) {
      comma = value.length();
    }
    String token = value.substring(start, comma);
    token.trim();
    if (token.length() > 0) {
      uint32_t pin = 0;
      if (parseUint32(token, pin)) {
        pins.push_back(static_cast<int>(pin));
      }
    }
    start = comma + 1;
  }
  return pins;
}

String normalizeGpioList(const std::vector<int> &pins) {
  String value;
  for (size_t i = 0; i < pins.size(); ++i) {
    if (i > 0) {
      value += ",";
    }
    value += String(pins[i]);
  }
  return value;
}

void handleCommand(const std::vector<String> &tokens) {
  if (tokens.empty()) {
    return;
  }
  String cmd = tokens[0];
  cmd.toUpperCase();
  if (cmd == "SET_RATE" && tokens.size() >= 2) {
    uint32_t rate = tokens[1].toInt();
    if (!isValidRate(rate)) {
      queueStatus("ERROR RATE INVALID");
      return;
    }
    configuredBaud = rate;
    tx.setBaud(configuredBaud);
    saveSettings();
    queueStatus("STATUS BAUD=" + String(configuredBaud));
    return;
  }
  if (cmd == "SET_INVERT" && tokens.size() >= 2) {
    configuredInvert = tokens[1].toInt() != 0;
    tx.setInvert(configuredInvert);
    saveSettings();
    queueStatus("STATUS INVERT=" + String(configuredInvert ? 1 : 0));
    return;
  }
  if (cmd == "SET_IDLE" && tokens.size() >= 2) {
    configuredIdleHigh = tokens[1].toInt() != 0;
    tx.setIdleHigh(configuredIdleHigh);
    saveSettings();
    queueStatus("STATUS IDLE=" + String(configuredIdleHigh ? 1 : 0));
    return;
  }
  if (cmd == "SET_MODE" && tokens.size() >= 2) {
    String value = tokens[1];
    value.toUpperCase();
    if (value == "OPENDRAIN" || value == "OPEN_DRAIN" || value == "OPEN_COLLECTOR") {
      configuredOutputMode = OutputMode::kOpenDrain;
    } else if (value == "PUSHPULL" || value == "PUSH_PULL") {
      configuredOutputMode = OutputMode::kPushPull;
    } else {
      queueStatus("ERROR MODE INVALID");
      return;
    }
    tx.setOutputMode(configuredOutputMode);
    saveSettings();
    queueStatus("STATUS OUTPUT=" +
                String(configuredOutputMode == OutputMode::kPushPull ? "PUSH_PULL"
                                                                    : "OPEN_DRAIN"));
    return;
  }
  if (cmd == "SET_GPIO" && tokens.size() >= 2) {
    uint32_t pin = 0;
    if (!parseUint32(tokens[1], pin)) {
      queueStatus("ERROR GPIO INVALID");
      return;
    }
    if (tx.isBusy()) {
      queueStatus("ERROR GPIO BUSY");
      return;
    }
    configuredDataGpio = static_cast<int>(pin);
    tx.begin(configuredDataGpio, configuredInvert, configuredBaud, configuredOutputMode,
             configuredIdleHigh);
    saveSettings();
    queueStatus(buildStatusLine());
    return;
  }
  if (cmd == "SET_GPIO_LIST" && tokens.size() >= 2) {
    String listValue = tokens[1];
    for (size_t i = 2; i < tokens.size(); ++i) {
      listValue += tokens[i];
    }
    std::vector<int> pins = parseGpioList(listValue);
    if (pins.empty()) {
      queueStatus("ERROR GPIO_LIST INVALID");
      return;
    }
    configuredGpioList = normalizeGpioList(pins);
    saveSettings();
    queueStatus("STATUS GPIO_LIST=" + configuredGpioList);
    return;
  }
  if (cmd == "SEND_TEST") {
    std::vector<uint8_t> bits = buildTestPattern(configuredCapcodeInd, 0, "TEST");
    enqueueRawBits(std::move(bits), "TEST_PATTERN");
    queueStatus("STATUS TEST QUEUED");
    return;
  }
  if (cmd == "DEBUG_SCOPE") {
    if (tx.isBusy()) {
      queueStatus("ERROR DEBUG_SCOPE TX_BUSY");
      return;
    }
    std::vector<uint8_t> bits = buildAlternatingBitsForBaud(2000, configuredBaud);
    tx.setOutputMode(configuredOutputMode);
    tx.setInvert(configuredInvert);
    tx.setIdleHigh(configuredIdleHigh);
    tx.setBaud(configuredBaud);
    if (!tx.sendBits(std::move(bits))) {
      queueStatus("ERROR DEBUG_SCOPE TX_BUSY");
      return;
    }
    pendingDebugScope = true;
    queueStatus("STATUS DEBUG_SCOPE START");
    return;
  }
  if (cmd == "SEND_MIN" && tokens.size() >= 3) {
    uint32_t capcode = 0;
    uint8_t functionBits = 0;
    uint32_t preambleMs = configuredDefaultPreambleMs;
    if (!parseUint32(tokens[1], capcode) || !parseFunctionBits(tokens[2], functionBits)) {
      queueStatus("ERROR SEND_MIN INVALID");
      return;
    }
    if (tokens.size() >= 4) {
      if (!parseUint32(tokens[3], preambleMs) || preambleMs == 0) {
        queueStatus("ERROR SEND_MIN PREAMBLE");
        return;
      }
    }
    if (tx.isBusy()) {
      queueStatus("ERROR SEND_MIN TX_BUSY");
      return;
    }
    configuredDefaultFunction = functionBits;
    configuredDefaultPreambleMs = preambleMs;
    if (!send_min_page(capcode, functionBits, preambleMs)) {
      queueStatus("ERROR SEND_MIN TX_BUSY");
      return;
    }
    queueStatus("STATUS MIN START capcode=" + String(capcode) +
                " func=" + String(functionBits) + " preamble=" + String(preambleMs) + "ms");
    return;
  }
  if (cmd == "SEND_MIN_LOOP" && tokens.size() >= 5) {
    uint32_t capcode = 0;
    uint8_t functionBits = 0;
    uint32_t preambleMs = 0;
    uint32_t durationSeconds = 0;
    if (!parseUint32(tokens[1], capcode) || !parseFunctionBits(tokens[2], functionBits) ||
        !parseUint32(tokens[3], preambleMs) || preambleMs == 0 ||
        !parseUint32(tokens[4], durationSeconds) || durationSeconds == 0) {
      queueStatus("ERROR SEND_MIN_LOOP INVALID");
      return;
    }
    if (autotest.isActive() || autotest2.isActive() || autotestFast.isActive()) {
      queueStatus("ERROR SEND_MIN_LOOP BUSY");
      return;
    }
    if (minLoop.isActive()) {
      queueStatus("ERROR SEND_MIN_LOOP BUSY");
      return;
    }
    if (tx.isBusy()) {
      queueStatus("ERROR SEND_MIN_LOOP TX_BUSY");
      return;
    }
    if (probe.isActive()) {
      probe.stop();
    }
    configuredDefaultFunction = functionBits;
    configuredDefaultPreambleMs = preambleMs;
    minLoop.start(capcode, functionBits, preambleMs, durationSeconds);
    queueStatus("STATUS SEND_MIN_LOOP START capcode=" + String(capcode) +
                " func=" + String(functionBits) + " preamble=" + String(preambleMs) + "ms");
    return;
  }
  if (cmd == "SEND_SYNC") {
    std::vector<uint32_t> codewords = {kSyncWord, kIdleWord, kIdleWord};
    std::vector<uint8_t> bits = encoder.buildBitstreamFromCodewords(codewords, true);
    enqueueRawBits(std::move(bits), "SYNC_ONLY");
    queueStatus("STATUS SYNC QUEUED");
    return;
  }
  if (cmd == "AUTOTEST" && tokens.size() >= 2) {
    String value = tokens[1];
    value.toUpperCase();
    if (value == "STOP") {
      if (!autotest.isActive()) {
        queueStatus("ERROR AUTOTEST NOT_ACTIVE");
        return;
      }
      autotest.requestStop();
      if (!tx.isBusy()) {
        autotest.update();
      }
      return;
    }
    uint32_t capcode = 0;
    if (!parseUint32(tokens[1], capcode)) {
      queueStatus("ERROR AUTOTEST INVALID");
      return;
    }
    uint32_t durationSeconds = 60;
    if (tokens.size() >= 3) {
      if (!parseUint32(tokens[2], durationSeconds) || durationSeconds == 0) {
        queueStatus("ERROR AUTOTEST DURATION");
        return;
      }
    }
    if (autotest.isActive()) {
      queueStatus("ERROR AUTOTEST BUSY");
      return;
    }
    if (tx.isBusy()) {
      queueStatus("ERROR AUTOTEST TX_BUSY");
      return;
    }
    if (probe.isActive()) {
      probe.stop();
    }
    autotest.start(capcode, durationSeconds, configuredBaud, configuredInvert,
                   configuredIdleHigh);
    queueStatus("STATUS AUTOTEST START");
    return;
  }
  if (cmd == "AUTOTEST_FAST" && tokens.size() >= 2) {
    String value = tokens[1];
    value.toUpperCase();
    if (value == "STOP") {
      if (!autotestFast.isActive()) {
        queueStatus("ERROR AUTOTEST_FAST NOT_ACTIVE");
        return;
      }
      autotestFast.requestStop();
      if (!tx.isBusy()) {
        autotestFast.update();
      }
      return;
    }
    uint32_t durationSeconds = 60;
    if (!parseUint32(tokens[1], durationSeconds) || durationSeconds == 0) {
      queueStatus("ERROR AUTOTEST_FAST DURATION");
      return;
    }
    if (autotestFast.isActive() || autotest.isActive() || autotest2.isActive() ||
        minLoop.isActive()) {
      queueStatus("ERROR AUTOTEST_FAST BUSY");
      return;
    }
    if (tx.isBusy()) {
      queueStatus("ERROR AUTOTEST_FAST TX_BUSY");
      return;
    }
    if (probe.isActive()) {
      probe.stop();
    }
    autotestFast.start(durationSeconds, configuredBaud, configuredInvert, configuredIdleHigh,
                       configuredOutputMode);
    queueStatus("STATUS AUTOTEST_FAST START capcode=" + String(kDefaultCapcodeInd) +
                " baud=512");
    return;
  }
  if (cmd == "AUTOTEST2" && tokens.size() >= 2) {
    String value = tokens[1];
    value.toUpperCase();
    if (value == "STOP") {
      if (!autotest2.isActive()) {
        queueStatus("ERROR AUTOTEST2 NOT_ACTIVE");
        return;
      }
      autotest2.requestStop();
      if (!tx.isBusy()) {
        autotest2.update();
      }
      return;
    }
    uint32_t capcode = 0;
    if (!parseUint32(tokens[1], capcode)) {
      queueStatus("ERROR AUTOTEST2 INVALID");
      return;
    }
    uint32_t durationSeconds = 60;
    if (tokens.size() >= 3) {
      if (!parseUint32(tokens[2], durationSeconds) || durationSeconds == 0) {
        queueStatus("ERROR AUTOTEST2 DURATION");
        return;
      }
    }
    if (autotest2.isActive()) {
      queueStatus("ERROR AUTOTEST2 BUSY");
      return;
    }
    if (autotest.isActive()) {
      queueStatus("ERROR AUTOTEST BUSY");
      return;
    }
    if (tx.isBusy()) {
      queueStatus("ERROR AUTOTEST2 TX_BUSY");
      return;
    }
    if (probe.isActive()) {
      probe.stop();
    }
    autotest2.start(capcode, durationSeconds, configuredBaud, configuredInvert, configuredIdleHigh,
                    configuredOutputMode, configuredDataGpio, configuredGpioList);
    queueStatus("STATUS AUTOTEST2 START");
    return;
  }
  if (cmd == "SEND_ADDR" && tokens.size() >= 3) {
    uint32_t capcode = 0;
    uint8_t functionBits = 0;
    if (!parseUint32(tokens[1], capcode) || !parseFunctionBits(tokens[2], functionBits)) {
      queueStatus("ERROR SEND_ADDR INVALID");
      return;
    }
    std::vector<uint8_t> bits = encoder.buildSingleBatchAddressOnly(capcode, functionBits);
    enqueueRawBits(std::move(bits), "ADDR_ONLY");
    queueStatus("STATUS ADDR QUEUED");
    return;
  }
  if (cmd == "SEND_MSG" && tokens.size() >= 4) {
    uint32_t capcode = 0;
    uint8_t functionBits = 0;
    if (!parseUint32(tokens[1], capcode) || !parseFunctionBits(tokens[2], functionBits)) {
      queueStatus("ERROR SEND_MSG INVALID");
      return;
    }
    String message;
    for (size_t i = 3; i < tokens.size(); ++i) {
      if (i > 3) {
        message += " ";
      }
      message += tokens[i];
    }
    std::vector<uint8_t> bits = encoder.buildSingleBatch(capcode, functionBits, message);
    enqueueRawBits(std::move(bits), "MSG_SINGLE");
    queueStatus("STATUS MSG QUEUED");
    return;
  }
  if (cmd == "SEND_CODEWORDS" && tokens.size() >= 2) {
    std::vector<uint32_t> codewords;
    codewords.reserve(tokens.size() - 1);
    for (size_t i = 1; i < tokens.size(); ++i) {
      uint32_t value = static_cast<uint32_t>(strtoul(tokens[i].c_str(), nullptr, 0));
      codewords.push_back(value);
    }
    std::vector<uint8_t> bits = encoder.buildBitstreamFromCodewords(codewords, true);
    enqueueRawBits(std::move(bits), "CODEWORDS");
    queueStatus("STATUS CODEWORDS QUEUED");
    return;
  }
  if (cmd == "SET" && tokens.size() >= 3) {
    String key = tokens[1];
    key.toUpperCase();
    if (key == "CAPCODE") {
      configuredCapcodeInd = tokens[2].toInt();
      if (!capGrpWasExplicitlySet) {
        configuredCapcodeGrp = configuredCapcodeInd + 1;
      }
      saveSettings();
      queueStatus("STATUS CAPS CAPIND=" + String(configuredCapcodeInd) +
                  " CAPGRP=" + String(configuredCapcodeGrp));
      return;
    }
    if (key == "CAPIND") {
      configuredCapcodeInd = tokens[2].toInt();
      saveSettings();
      queueStatus("STATUS CAPIND=" + String(configuredCapcodeInd));
      return;
    }
    if (key == "CAPGRP") {
      configuredCapcodeGrp = tokens[2].toInt();
      capGrpWasExplicitlySet = true;
      saveSettings();
      queueStatus("STATUS CAPGRP=" + String(configuredCapcodeGrp));
      return;
    }
    if (key == "CAPS" && tokens.size() >= 4) {
      configuredCapcodeInd = tokens[2].toInt();
      configuredCapcodeGrp = tokens[3].toInt();
      capGrpWasExplicitlySet = true;
      saveSettings();
      queueStatus("STATUS CAPS CAPIND=" + String(configuredCapcodeInd) +
                  " CAPGRP=" + String(configuredCapcodeGrp));
      return;
    }
    if (key == "BAUD") {
      uint32_t rate = tokens[2].toInt();
      if (!isValidRate(rate)) {
        queueStatus("ERROR BAUD INVALID");
        return;
      }
      configuredBaud = rate;
      tx.setBaud(configuredBaud);
      saveSettings();
      queueStatus("STATUS BAUD=" + String(configuredBaud));
      return;
    }
    if (key == "INVERT") {
      configuredInvert = tokens[2].toInt() != 0;
      tx.setInvert(configuredInvert);
      saveSettings();
      queueStatus("STATUS INVERT=" + String(configuredInvert ? 1 : 0));
      return;
    }
    if (key == "IDLE" && tokens.size() >= 3) {
      configuredIdleHigh = tokens[2].toInt() != 0;
      tx.setIdleHigh(configuredIdleHigh);
      saveSettings();
      queueStatus("STATUS IDLE=" + String(configuredIdleHigh ? 1 : 0));
      return;
    }
    if (key == "OUTPUT") {
      String value = tokens[2];
      value.toUpperCase();
      if (value == "OPEN_DRAIN" || value == "OPEN_COLLECTOR") {
        configuredOutputMode = OutputMode::kOpenDrain;
      } else if (value == "PUSH_PULL") {
        configuredOutputMode = OutputMode::kPushPull;
      } else {
        queueStatus("ERROR OUTPUT INVALID");
        return;
      }
      tx.setOutputMode(configuredOutputMode);
      saveSettings();
      queueStatus("STATUS OUTPUT=" +
                  String(configuredOutputMode == OutputMode::kPushPull ? "PUSH_PULL"
                                                                      : "OPEN_DRAIN"));
      return;
    }
    if (key == "AUTOPROBE") {
      configuredAutoProbe = tokens[2].toInt() != 0;
      saveSettings();
      queueStatus("STATUS AUTOPROBE=" + String(configuredAutoProbe ? 1 : 0));
      return;
    }
  }

  if (cmd == "PAGE" && tokens.size() >= 2) {
    auto isNumber = [](const String &value) {
      if (value.length() == 0) {
        return false;
      }
      for (size_t i = 0; i < value.length(); ++i) {
        if (!isdigit(static_cast<unsigned char>(value[i]))) {
          return false;
        }
      }
      return true;
    };
    if (tokens.size() >= 3 && isNumber(tokens[1])) {
      uint32_t capcode = tokens[1].toInt();
      String message;
      for (size_t i = 2; i < tokens.size(); ++i) {
        if (i > 2) {
          message += " ";
        }
        message += tokens[i];
      }
      enqueuePage(capcode, message, true);
      queueStatus("STATUS PAGE QUEUED");
      return;
    }
    String message;
    for (size_t i = 1; i < tokens.size(); ++i) {
      if (i > 1) {
        message += " ";
      }
      message += tokens[i];
    }
    enqueuePage(configuredCapcodeInd, message, true);
    queueStatus("STATUS PAGE QUEUED");
    return;
  }

  if (cmd == "PAGEI" && tokens.size() >= 2) {
    String message;
    for (size_t i = 1; i < tokens.size(); ++i) {
      if (i > 1) {
        message += " ";
      }
      message += tokens[i];
    }
    enqueuePage(configuredCapcodeInd, message, true);
    queueStatus("STATUS PAGE QUEUED");
    return;
  }

  if (cmd == "PAGEG" && tokens.size() >= 2) {
    String message;
    for (size_t i = 1; i < tokens.size(); ++i) {
      if (i > 1) {
        message += " ";
      }
      message += tokens[i];
    }
    enqueuePage(configuredCapcodeGrp, message, true);
    queueStatus("STATUS PAGE QUEUED");
    return;
  }

  if (cmd == "TEST" && tokens.size() >= 3) {
    String mode = tokens[1];
    mode.toUpperCase();
    if (mode == "CARRIER") {
      uint32_t durationMs = tokens[2].toInt();
      if (durationMs == 0) {
        queueStatus("ERROR TEST DURATION");
        return;
      }
      enqueueCarrier(durationMs);
      queueStatus("STATUS TEST CARRIER QUEUED");
      return;
    }
  }

  if (cmd == "LIST") {
    for (const auto &line : pageStore.listSummary()) {
      queueStatus(line);
    }
    return;
  }

  if (cmd == "RESEND" && tokens.size() >= 2) {
    PageRecord record;
    size_t index = tokens[1].toInt();
    if (pageStore.getByIndex(index, record)) {
      enqueuePage(record.capcode, record.message, false);
      queueStatus("STATUS RESEND QUEUED");
    } else {
      queueStatus("ERROR RESEND BAD_INDEX");
    }
    return;
  }

  if (cmd == "STATUS") {
    queueStatus(buildStatusLine());
    return;
  }

  if (cmd == "SAVE") {
    saveSettings();
    queueStatus("STATUS SAVED");
    return;
  }

  if (cmd == "CLEAR") {
    pageStore.clear();
    saveSettings();
    queueStatus("STATUS CLEARED");
    return;
  }

  if (cmd == "PROBE" && tokens.size() >= 2) {
    String mode = tokens[1];
    mode.toUpperCase();
    if (mode == "STOP") {
      probe.stop();
      queueStatus("STATUS PROBE STOP");
      return;
    }
    if (mode == "START" && tokens.size() >= 5) {
      uint32_t startCap = tokens[2].toInt();
      uint32_t endCap = tokens[3].toInt();
      uint32_t step = tokens[4].toInt();
      if (probe.startSequential(startCap, endCap, step)) {
        queueStatus("STATUS PROBE START");
      } else {
        queueStatus("ERROR PROBE NO_ALERT_GPIO");
      }
      return;
    }
    if (mode == "BINARY" && tokens.size() >= 4) {
      uint32_t startCap = tokens[2].toInt();
      uint32_t endCap = tokens[3].toInt();
      if (probe.startBinary(startCap, endCap)) {
        queueStatus("STATUS PROBE BINARY");
      } else {
        queueStatus("ERROR PROBE NO_ALERT_GPIO");
      }
      return;
    }
    if (mode == "ONESHOT" && tokens.size() >= 3) {
      std::vector<uint32_t> caps;
      for (size_t i = 2; i < tokens.size(); ++i) {
        uint32_t cap = tokens[i].toInt();
        if (cap > 0) {
          caps.push_back(cap);
        }
      }
      if (caps.empty()) {
        queueStatus("ERROR PROBE ONESHOT EMPTY");
        return;
      }
      probe.startOneshot(caps);
      queueStatus("STATUS PROBE ONESHOT");
      return;
    }
  }

  queueStatus("ERROR UNKNOWN_CMD");
}

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server) override {
    (void)server;
    Serial.println("BLE connected.");
  }

  void onDisconnect(NimBLEServer *server) override {
    (void)server;
    Serial.println("BLE disconnected.");
    NimBLEDevice::startAdvertising();
  }
};

class RxCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic) override {
    std::string value = characteristic->getValue();
    String input(value.c_str());
    parser.handleInput(input);
  }
};
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("PagerBridge booting...");

  preferences.begin("pager", false);
  bool hasCapInd = preferences.isKey("cap_ind");
  bool hasCapGrp = preferences.isKey("cap_grp");
  bool hasLegacyCap = preferences.isKey("capcode");
  configuredCapcodeInd = preferences.getUInt("cap_ind", kDefaultCapcodeInd);
  configuredCapcodeGrp = preferences.getUInt("cap_grp", kDefaultCapcodeGrp);
  bool migratedLegacy = false;
  if (hasLegacyCap && !hasCapInd && !hasCapGrp) {
    uint32_t legacyCapcode = preferences.getUInt("capcode", kDefaultCapcodeInd);
    configuredCapcodeInd = legacyCapcode;
    if (configuredCapcodeInd == 0) {
      configuredCapcodeGrp = kDefaultCapcodeGrp;
    } else {
      configuredCapcodeGrp = configuredCapcodeInd + 1;
    }
    migratedLegacy = true;
  }
  configuredBaud = preferences.getUInt("baud", kDefaultBaud);
  configuredInvert = preferences.getBool("invert", kDefaultInvert);
  uint8_t outputValue =
      preferences.getUChar("output", static_cast<uint8_t>(kDefaultOutputMode));
  configuredOutputMode =
      outputValue == static_cast<uint8_t>(OutputMode::kOpenDrain) ? OutputMode::kOpenDrain
                                                                   : OutputMode::kPushPull;
  bool hasDataGpio = preferences.isKey("dataGpio");
  bool hasIdleHigh = preferences.isKey("idleHigh");
  configuredDataGpio =
      hasDataGpio ? preferences.getInt("dataGpio", kDataGpio)
                  : preferences.getInt("data_gpio", kDataGpio);
  configuredGpioList = preferences.getString("gpioList", "");
  configuredIdleHigh =
      hasIdleHigh ? preferences.getBool("idleHigh", kDefaultIdleLineHigh)
                  : preferences.getBool("idle_high", kDefaultIdleLineHigh);
  configuredAutoProbe = preferences.getBool("autoProbe", false);
  pageStore.load(preferences);
  if (migratedLegacy) {
    saveSettings();
  }

  tx.begin(configuredDataGpio, configuredInvert, configuredBaud, configuredOutputMode,
           configuredIdleHigh);
  probe.begin(kAlertGpio);

  parser.setHandler(handleCommand);

  NimBLEDevice::init(kDeviceName);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  NimBLEService *service = bleServer->createService(kServiceUUID);
  rxCharacteristic = service->createCharacteristic(kRxUUID, NIMBLE_PROPERTY::WRITE);
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
  Serial.println("PagerBridge ready.");
  Serial.println("Settings:");
  Serial.println("  CAPIND=" + String(configuredCapcodeInd));
  Serial.println("  CAPGRP=" + String(configuredCapcodeGrp));
  Serial.println("  BAUD=" + String(configuredBaud));
  Serial.println("  INVERT=" + String(configuredInvert ? 1 : 0));
  Serial.println("  IDLE=" + String(configuredIdleHigh ? 1 : 0));
  Serial.println("  OUTPUT=" +
                 String(configuredOutputMode == OutputMode::kPushPull ? "PUSH_PULL"
                                                                      : "OPEN_DRAIN"));
  Serial.println("  DATA_GPIO=" + String(configuredDataGpio));
  Serial.println("  ALERT_GPIO=" + String(kAlertGpio));
  Serial.println("  AUTOPROBE=" + String(configuredAutoProbe ? 1 : 0));
  queueStatus("READY");
  if (configuredAutoProbe) {
    std::vector<uint32_t> caps;
    caps.push_back(configuredCapcodeInd);
    if (configuredCapcodeGrp != configuredCapcodeInd) {
      caps.push_back(configuredCapcodeGrp);
    }
    probe.startOneshot(caps);
  }
}

void loop() {
  if (tx.consumeDoneFlag()) {
    queueStatus("TX_DONE");
    if (pendingDebugScope) {
      pendingDebugScope = false;
      queueStatus("DEBUG_SCOPE done");
    }
    if (pendingStoredPage) {
      pageStore.updateLatestResult("TX_DONE");
      saveSettings();
      pendingStoredPage = false;
    }
  }

  if (minLoop.isActive()) {
    minLoop.update();
  } else if (autotestFast.isActive()) {
    autotestFast.update();
  } else if (autotest2.isActive()) {
    autotest2.update();
  } else if (autotest.isActive()) {
    autotest.update();
  } else {
    if (!tx.isBusy() && !txQueue.empty() && !probe.isActive()) {
      TxRequest request = txQueue.front();
      txQueue.pop_front();
      if (request.isRaw) {
        if (tx.sendBits(std::move(request.rawBits))) {
          queueStatus("TX_START " + request.label);
        } else {
          queueStatus("ERROR TX_BUSY");
        }
        return;
      }
      auto bits = encoder.buildBitstream(request.capcode, request.message);
      if (tx.sendBits(std::move(bits))) {
        queueStatus("TX_START capcode=" + String(request.capcode));
        if (request.store) {
          pageStore.add(request.capcode, request.message, millis(), "TX_START");
          pendingStoredPage = true;
        }
      } else {
        queueStatus("ERROR TX_BUSY");
      }
    }

    if (probe.isActive()) {
      probe.update(
          [](uint32_t capcode) {
            configuredCapcodeInd = capcode;
            if (!capGrpWasExplicitlySet) {
              configuredCapcodeGrp = capcode + 1;
            }
            saveSettings();
            pageStore.add(capcode, "PROBE_HIT capcode=" + String(capcode), millis(),
                          "PROBE_HIT");
            saveSettings();
            queueStatus("PROBE_HIT capcode=" + String(capcode));
          },
          [](uint32_t capcode) {
            queueStatus("PROBE_STEP capcode=" + String(capcode));
            queueStatus("TX_START capcode=" + String(capcode));
          });
    }
  }

  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      if (serialBuffer.length() > 0) {
        parser.handleInput(serialBuffer);
        serialBuffer = "";
      }
      continue;
    }
    serialBuffer += c;
  }

  notifyStatus();
  delay(5);
}
