#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
}

#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_pm.h"
#include "esp_private/esp_clk.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

namespace {
constexpr char kTag[] = "pocsag_tx";
constexpr char kBleDeviceName[] = "PagerBridge";
constexpr char kServiceUuidStr[] = "1b0ee9b4-e833-5a9e-354c-7e2d486b2b7f";
constexpr char kRxUuidStr[] = "1b0ee9b4-e833-5a9e-354c-7e2d496b2b7f";
constexpr char kStatusUuidStr[] = "1b0ee9b4-e833-5a9e-354c-7e2d4a6b2b7f";
constexpr int kUserLedGpio = 21;            // XIAO ESP32S3 LED_BUILTIN
constexpr bool kUserLedActiveHigh = false;  // XIAO user LED is active-low
constexpr uint32_t kUserLedBootOnMs = 10000;
constexpr uint32_t kUserLedHeartbeatPeriodMs = 15000;
constexpr uint32_t kUserLedHeartbeatPulseMs = 150;
constexpr uint32_t kPmArmDelayMs = 10000;   // stay fully awake for initial debug window
constexpr int kPmMaxFreqMhz = 80;
constexpr int kPmMinFreqMhz = 40;
constexpr bool kPmLightSleepEnable = false;
constexpr esp_power_level_t kBleTxPowerDefault = ESP_PWR_LVL_N0;  // 0 dBm
constexpr uint32_t kMetricsLogPeriodMs = 60000;
constexpr uint32_t kCpuSamplePeriodMs = 1000;
constexpr uint32_t kSyncWord = 0x7CD215D8;
constexpr uint32_t kIdleWord = 0x7A89C197;
constexpr uint32_t kMaxRmtDuration = 32767;
constexpr size_t kMaxRmtItems = 2000;
constexpr uint16_t kAdvFastIntervalMin = 0x0140;  // 200 ms
constexpr uint16_t kAdvFastIntervalMax = 0x01E0;  // 300 ms
constexpr int32_t kAdvFastDurationMs = 15000;
constexpr uint16_t kAdvSlowIntervalMin = 0x0C80;  // 2.0 s
constexpr uint16_t kAdvSlowIntervalMax = 0x12C0;  // 3.0 s

const ble_uuid128_t kServiceUuid = BLE_UUID128_INIT(
    0x7f, 0x2b, 0x6b, 0x48, 0x2d, 0x7e, 0x4c, 0x35, 0x9e, 0x5a, 0x33, 0xe8, 0xb4, 0xe9, 0x0e, 0x1b);
const ble_uuid128_t kRxUuid = BLE_UUID128_INIT(
    0x7f, 0x2b, 0x6b, 0x49, 0x2d, 0x7e, 0x4c, 0x35, 0x9e, 0x5a, 0x33, 0xe8, 0xb4, 0xe9, 0x0e, 0x1b);
const ble_uuid128_t kStatusUuid = BLE_UUID128_INIT(
    0x7f, 0x2b, 0x6b, 0x4a, 0x2d, 0x7e, 0x4c, 0x35, 0x9e, 0x5a, 0x33, 0xe8, 0xb4, 0xe9, 0x0e, 0x1b);

enum class InputSource : uint8_t { kSerial = 0, kBle = 1 };
enum class AdvProfile : uint8_t { kFastReconnect = 0, kSlowIdle = 1 };

struct AdvProfileConfig {
  uint16_t intervalMin;
  uint16_t intervalMax;
  int32_t durationMs;
  const char* label;
};

struct RuntimeMetrics {
  uint64_t bootUs = 0;
  uint64_t connStateSinceUs = 0;
  uint64_t advStateSinceUs = 0;
  uint64_t connectedUs = 0;
  uint64_t disconnectedUs = 0;
  uint64_t advertisingUs = 0;
  bool connected = false;
  bool advertising = false;
};

struct CpuMetrics {
  uint64_t samples = 0;
  uint64_t mhz40 = 0;
  uint64_t mhz80 = 0;
  uint64_t mhz160 = 0;
  uint64_t mhz240 = 0;
  uint64_t mhzOther = 0;
};

static AdvProfileConfig get_adv_profile_config(AdvProfile profile) {
  if (profile == AdvProfile::kSlowIdle) {
    return {kAdvSlowIntervalMin, kAdvSlowIntervalMax, BLE_HS_FOREVER, "slow-idle"};
  }
  return {kAdvFastIntervalMin, kAdvFastIntervalMax, kAdvFastDurationMs, "fast-reconnect"};
}
}

enum class OutputMode : uint8_t { kOpenDrain = 0, kPushPull = 1 };

struct Config {
  uint32_t baud = 512;
  uint32_t preambleBits = 576;
  uint32_t capInd = 1422890;
  uint8_t functionBits = 2;
  int dataGpio = 4;
  OutputMode output = OutputMode::kPushPull;
  bool invertWords = false;
  bool driveOneLow = true;
  bool idleHigh = true;
};

static Config gConfig;

struct TxJob {
  std::vector<uint8_t> bits;
};

static QueueHandle_t gTxQueue = nullptr;
static uint8_t gBleAddrType = 0;
static uint16_t gBleConnHandle = BLE_HS_CONN_HANDLE_NONE;
static bool gBleAdvertising = false;
static AdvProfile gAdvProfile = AdvProfile::kFastReconnect;
static esp_power_level_t gBleTxPowerTarget = kBleTxPowerDefault;
static bool gPmConfigured = false;
static bool gPmConfigureAttempted = false;
static esp_err_t gPmConfigureErr = ESP_OK;
static bool gBleAddrValid = false;
static uint8_t gBleAddr[6] = {};
static RuntimeMetrics gMetrics;
static CpuMetrics gCpuMetrics;
static portMUX_TYPE gMetricsMux = portMUX_INITIALIZER_UNLOCKED;

static void process_input_payload(const std::string& payload, InputSource source);
static int ble_gap_event(struct ble_gap_event* event, void* arg);
static void start_ble_advertising(AdvProfile profile);
static void log_ble_status();
static void log_runtime_metrics(const char* reason);
static void log_pm_locks();
static void configure_ble_tx_power();
static bool parse_ble_tx_power_dbm(const std::string& token, esp_power_level_t* outLevel);
static void log_ble_tx_power_status();

static int ble_tx_power_dbm(esp_power_level_t level) {
  switch (level) {
    case ESP_PWR_LVL_N24: return -24;
    case ESP_PWR_LVL_N21: return -21;
    case ESP_PWR_LVL_N18: return -18;
    case ESP_PWR_LVL_N15: return -15;
    case ESP_PWR_LVL_N12: return -12;
    case ESP_PWR_LVL_N9: return -9;
    case ESP_PWR_LVL_N6: return -6;
    case ESP_PWR_LVL_N3: return -3;
    case ESP_PWR_LVL_N0: return 0;
    case ESP_PWR_LVL_P3: return 3;
    case ESP_PWR_LVL_P6: return 6;
    case ESP_PWR_LVL_P9: return 9;
    case ESP_PWR_LVL_P12: return 12;
    case ESP_PWR_LVL_P15: return 15;
    case ESP_PWR_LVL_P18: return 18;
    case ESP_PWR_LVL_P20: return 20;
    default: return 999;
  }
}

static bool parse_ble_tx_power_dbm(const std::string& token, esp_power_level_t* outLevel) {
  if (outLevel == nullptr) {
    return false;
  }
  if (token.empty()) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const long parsed = std::strtol(token.c_str(), &end, 10);
  if (errno != 0 || end == token.c_str() || *end != '\0') {
    return false;
  }
  if (parsed < -24 || parsed > 20) {
    return false;
  }
  const int dbm = static_cast<int>(parsed);
  switch (dbm) {
    case -24: *outLevel = ESP_PWR_LVL_N24; return true;
    case -21: *outLevel = ESP_PWR_LVL_N21; return true;
    case -18: *outLevel = ESP_PWR_LVL_N18; return true;
    case -15: *outLevel = ESP_PWR_LVL_N15; return true;
    case -12: *outLevel = ESP_PWR_LVL_N12; return true;
    case -9: *outLevel = ESP_PWR_LVL_N9; return true;
    case -6: *outLevel = ESP_PWR_LVL_N6; return true;
    case -3: *outLevel = ESP_PWR_LVL_N3; return true;
    case 0: *outLevel = ESP_PWR_LVL_N0; return true;
    case 3: *outLevel = ESP_PWR_LVL_P3; return true;
    case 6: *outLevel = ESP_PWR_LVL_P6; return true;
    case 9: *outLevel = ESP_PWR_LVL_P9; return true;
    case 12: *outLevel = ESP_PWR_LVL_P12; return true;
    case 15: *outLevel = ESP_PWR_LVL_P15; return true;
    case 18: *outLevel = ESP_PWR_LVL_P18; return true;
    case 20: *outLevel = ESP_PWR_LVL_P20; return true;
    default: return false;
  }
}

static void log_ble_tx_power_status() {
  const esp_power_level_t advLevel = esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_ADV);
  const esp_power_level_t defaultLevel = esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_DEFAULT);
  ESP_LOGI(kTag, "txpower: target=%ddBm adv=%ddBm default=%ddBm",
           ble_tx_power_dbm(gBleTxPowerTarget), ble_tx_power_dbm(advLevel), ble_tx_power_dbm(defaultLevel));
}

static void cpu_metrics_sample() {
  const int mhz = esp_clk_cpu_freq() / 1000000;
  portENTER_CRITICAL(&gMetricsMux);
  gCpuMetrics.samples++;
  if (mhz <= 40) {
    gCpuMetrics.mhz40++;
  } else if (mhz <= 80) {
    gCpuMetrics.mhz80++;
  } else if (mhz <= 160) {
    gCpuMetrics.mhz160++;
  } else if (mhz <= 240) {
    gCpuMetrics.mhz240++;
  } else {
    gCpuMetrics.mhzOther++;
  }
  portEXIT_CRITICAL(&gMetricsMux);
}

static void set_user_led(bool on) {
  const int onLevel = kUserLedActiveHigh ? 1 : 0;
  const int offLevel = kUserLedActiveHigh ? 0 : 1;
  const esp_err_t err = gpio_set_level(static_cast<gpio_num_t>(kUserLedGpio), on ? onLevel : offLevel);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "User LED gpio_set_level failed: 0x%x", err);
  }
}

static void init_user_led() {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << static_cast<uint32_t>(kUserLedGpio);
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  cfg.mode = GPIO_MODE_OUTPUT;
  const esp_err_t cfgErr = gpio_config(&cfg);
  if (cfgErr != ESP_OK) {
    ESP_LOGW(kTag, "User LED gpio_config failed: 0x%x", cfgErr);
    return;
  }
  set_user_led(false);
}

static void user_led_task(void*) {
  set_user_led(true);
  vTaskDelay(pdMS_TO_TICKS(kUserLedBootOnMs));

  set_user_led(false);
  const TickType_t pulseTicks = pdMS_TO_TICKS(kUserLedHeartbeatPulseMs);
  const TickType_t idleTicks = pdMS_TO_TICKS(kUserLedHeartbeatPeriodMs - kUserLedHeartbeatPulseMs);
  while (true) {
    vTaskDelay(idleTicks);
    set_user_led(true);
    vTaskDelay(pulseTicks);
    set_user_led(false);
  }
}

static void set_idle_line(int gpio, OutputMode output, bool idleHigh) {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << gpio;
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  cfg.mode = output == OutputMode::kOpenDrain ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&cfg));
  ESP_ERROR_CHECK(gpio_set_level(static_cast<gpio_num_t>(gpio), idleHigh ? 1 : 0));
}

class WaveTx {
 public:
  ~WaveTx() { shutdown_rmt(); }

  bool transmit_bits(const std::vector<uint8_t>& bits, const Config& cfg) {
    if (busy_) {
      return false;
    }
    if (bits.empty()) {
      set_idle_line(cfg.dataGpio, cfg.output, cfg.idleHigh);
      return true;
    }

    const uint32_t bitPeriodUs = (1000000 + (cfg.baud / 2)) / cfg.baud;
    build_items(bits, bitPeriodUs, cfg.driveOneLow);
    if (items_.empty()) {
      return false;
    }

    busy_ = true;
    const bool configured = ensure_rmt(cfg.dataGpio, cfg.output, cfg.idleHigh);
    if (!configured) {
      busy_ = false;
      return false;
    }

    rmt_transmit_config_t tx_cfg = {};
    tx_cfg.loop_count = 0;
    tx_cfg.flags.eot_level = cfg.idleHigh ? 1 : 0;

    esp_err_t err = rmt_transmit(channel_, encoder_, items_.data(),
                                 items_.size() * sizeof(rmt_symbol_word_t), &tx_cfg);
    if (err == ESP_OK) {
      err = rmt_tx_wait_all_done(channel_, -1);
    }
    if (err != ESP_OK) {
      ESP_LOGE(kTag, "rmt_transmit failed: 0x%x", err);
    }

    shutdown_rmt();
    set_idle_line(cfg.dataGpio, cfg.output, cfg.idleHigh);
    busy_ = false;
    return err == ESP_OK;
  }

 private:
  bool ensure_rmt(int gpio, OutputMode output, bool idleHigh) {
    shutdown_rmt();

    set_idle_line(gpio, output, idleHigh);
    rmt_tx_channel_config_t tx_channel_cfg = {};
    tx_channel_cfg.gpio_num = static_cast<gpio_num_t>(gpio);
    tx_channel_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_channel_cfg.resolution_hz = 1000000;
    tx_channel_cfg.mem_block_symbols = 128;
    tx_channel_cfg.trans_queue_depth = 1;
    tx_channel_cfg.flags.io_od_mode = output == OutputMode::kOpenDrain;

    esp_err_t err = rmt_new_tx_channel(&tx_channel_cfg, &channel_);
    if (err != ESP_OK) {
      ESP_LOGE(kTag, "rmt_new_tx_channel failed: 0x%x", err);
      return false;
    }

    rmt_copy_encoder_config_t copy_encoder_cfg = {};
    err = rmt_new_copy_encoder(&copy_encoder_cfg, &encoder_);
    if (err != ESP_OK) {
      ESP_LOGE(kTag, "rmt_new_copy_encoder failed: 0x%x", err);
      shutdown_rmt();
      return false;
    }

    err = rmt_enable(channel_);
    if (err != ESP_OK) {
      ESP_LOGE(kTag, "rmt_enable failed: 0x%x", err);
      shutdown_rmt();
      return false;
    }

    initialized_ = true;
    return true;
  }

  void shutdown_rmt() {
    if (channel_ != nullptr) {
      const esp_err_t disableErr = rmt_disable(channel_);
      if (disableErr != ESP_OK && disableErr != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "rmt_disable failed: 0x%x", disableErr);
      }
      const esp_err_t delErr = rmt_del_channel(channel_);
      if (delErr != ESP_OK) {
        ESP_LOGW(kTag, "rmt_del_channel failed: 0x%x", delErr);
      }
      channel_ = nullptr;
    }

    if (encoder_ != nullptr) {
      const esp_err_t delErr = rmt_del_encoder(encoder_);
      if (delErr != ESP_OK) {
        ESP_LOGW(kTag, "rmt_del_encoder failed: 0x%x", delErr);
      }
      encoder_ = nullptr;
    }
    initialized_ = false;
  }

  void build_items(const std::vector<uint8_t>& bits, uint32_t bitPeriodUs, bool driveOneLow) {
    items_.clear();
    size_t index = 0;

    while (index < bits.size()) {
      const uint8_t value = bits[index];
      size_t runLength = 1;
      while ((index + runLength) < bits.size() && bits[index + runLength] == value) {
        ++runLength;
      }

      uint32_t totalDuration = static_cast<uint32_t>(runLength) * bitPeriodUs;
      const bool levelHigh = driveOneLow ? (value == 0) : (value != 0);
      while (totalDuration > 0) {
        const uint32_t chunk = totalDuration > kMaxRmtDuration ? kMaxRmtDuration : totalDuration;
        rmt_symbol_word_t item = {};
        item.duration0 = chunk > 1 ? chunk - 1 : 1;
        item.level0 = levelHigh;
        item.duration1 = 1;
        item.level1 = levelHigh;
        items_.push_back(item);

        if (items_.size() > kMaxRmtItems) {
          ESP_LOGE(kTag, "RMT item overflow");
          items_.clear();
          return;
        }
        totalDuration -= chunk;
      }
      index += runLength;
    }
  }

  rmt_channel_handle_t channel_ = nullptr;
  rmt_encoder_handle_t encoder_ = nullptr;
  bool initialized_ = false;
  bool busy_ = false;
  std::vector<rmt_symbol_word_t> items_;
};

class PocsagEncoder {
 public:
  std::vector<uint32_t> build_batch_words(uint32_t capcode, uint8_t functionBits,
                                          const std::string& message) const {
    std::vector<uint32_t> words(16, kIdleWord);
    const uint8_t frame = static_cast<uint8_t>(capcode & 0x7);
    size_t index = static_cast<size_t>(frame) * 2;
    words[index++] = build_address_word(capcode, functionBits);

    for (const uint32_t messageWord : build_alpha_words(message)) {
      if (index >= words.size()) {
        break;
      }
      words[index++] = messageWord;
    }
    return words;
  }

 private:
  std::vector<uint32_t> build_alpha_words(const std::string& message) const {
    std::vector<uint8_t> bits;
    bits.reserve(message.size() * 7);
    for (char c : message) {
      const uint8_t value = static_cast<uint8_t>(c) & 0x7F;
      for (int b = 0; b < 7; ++b) {
        bits.push_back((value >> b) & 0x1);
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
      words.push_back(encode_codeword((1u << 20) | (data & 0xFFFFF)));
    }
    if (words.empty()) {
      words.push_back(encode_codeword(1u << 20));
    }
    return words;
  }

  uint32_t build_address_word(uint32_t capcode, uint8_t functionBits) const {
    const uint32_t address = capcode >> 3;
    const uint32_t data = ((address & 0x3FFFF) << 2) | (functionBits & 0x3);
    return encode_codeword(data & 0x1FFFFF);
  }

  uint32_t encode_codeword(uint32_t msg21) const {
    uint32_t reg = msg21 << 10;
    constexpr uint32_t poly = 0x769;
    for (int i = 30; i >= 10; --i) {
      if (reg & (1u << i)) {
        reg ^= (poly << (i - 10));
      }
    }
    const uint32_t remainder = reg & 0x3FF;
    uint32_t word = (msg21 << 11) | (remainder << 1);
    word |= static_cast<uint32_t>(__builtin_parity(word));
    return word;
  }
};

static PocsagEncoder gEncoder;
static WaveTx gWaveTx;

static std::string trim_copy(const std::string& in) {
  size_t start = 0;
  while (start < in.size() && std::isspace(static_cast<unsigned char>(in[start])) != 0) {
    ++start;
  }
  size_t end = in.size();
  while (end > start && std::isspace(static_cast<unsigned char>(in[end - 1])) != 0) {
    --end;
  }
  return in.substr(start, end - start);
}

static std::string to_lower_copy(std::string in) {
  for (char& c : in) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return in;
}

static std::vector<uint8_t> build_pocsag_bits(const std::string& message, const Config& cfg) {
  std::vector<uint8_t> bits;
  bits.reserve(cfg.preambleBits + 544);

  for (uint32_t i = 0; i < cfg.preambleBits; ++i) {
    bits.push_back(static_cast<uint8_t>(i % 2 == 0));
  }

  uint32_t sync = cfg.invertWords ? ~kSyncWord : kSyncWord;
  for (int i = 31; i >= 0; --i) {
    bits.push_back((sync >> i) & 0x1);
  }

  auto words = gEncoder.build_batch_words(cfg.capInd, cfg.functionBits, message);
  for (uint32_t& word : words) {
    if (cfg.invertWords) {
      word = ~word;
    }
    for (int i = 31; i >= 0; --i) {
      bits.push_back((word >> i) & 0x1);
    }
  }
  return bits;
}

static bool enqueue_message_page(const std::string& message, TickType_t waitTicks) {
  const std::vector<uint8_t> bits = build_pocsag_bits(message, gConfig);
  TxJob* job = new TxJob{bits};
  if (xQueueSend(gTxQueue, &job, waitTicks) != pdTRUE) {
    delete job;
    ESP_LOGW(kTag, "Queue busy; dropped input");
    return false;
  }
  ESP_LOGI(kTag, "Queued: %s", message.c_str());
  return true;
}

static void log_status() {
  const UBaseType_t queued = gTxQueue == nullptr ? 0 : uxQueueMessagesWaiting(gTxQueue);
  ESP_LOGI(kTag, "status: capcode=%lu func=%u baud=%lu preamble=%lu",
           static_cast<unsigned long>(gConfig.capInd),
           static_cast<unsigned>(gConfig.functionBits),
           static_cast<unsigned long>(gConfig.baud),
           static_cast<unsigned long>(gConfig.preambleBits));
  ESP_LOGI(kTag, "status: gpio=%d output=%s idle=%s driveOneLow=%s invertWords=%s queue=%lu",
           gConfig.dataGpio,
           gConfig.output == OutputMode::kOpenDrain ? "open-drain" : "push-pull",
           gConfig.idleHigh ? "high" : "low",
           gConfig.driveOneLow ? "yes" : "no",
           gConfig.invertWords ? "yes" : "no",
           static_cast<unsigned long>(queued));
  ESP_LOGI(kTag, "status: ble connected=%s advertising=%s",
           gBleConnHandle == BLE_HS_CONN_HANDLE_NONE ? "no" : "yes",
           gBleAdvertising ? "yes" : "no");
  ESP_LOGI(kTag, "status: ble tx_power target=%ddBm", ble_tx_power_dbm(gBleTxPowerTarget));
  if (gBleAddrValid) {
    ESP_LOGI(kTag, "status: ble mac=%02x:%02x:%02x:%02x:%02x:%02x",
             gBleAddr[5], gBleAddr[4], gBleAddr[3], gBleAddr[2], gBleAddr[1], gBleAddr[0]);
  }
}

static void configure_power_management() {
#if CONFIG_PM_ENABLE
  gPmConfigureAttempted = true;
  esp_pm_config_t pm = {};
  pm.max_freq_mhz = kPmMaxFreqMhz;
  pm.min_freq_mhz = kPmMinFreqMhz;
  // Light sleep currently causes NimBLE host to become disabled on this target build.
  // Keep DFS enabled for savings while preserving BLE availability.
  pm.light_sleep_enable = kPmLightSleepEnable;
  const esp_err_t err = esp_pm_configure(&pm);
  gPmConfigureErr = err;
  if (err == ESP_OK) {
    gPmConfigured = true;
    ESP_LOGI(kTag, "Power management configured (%d-%dMHz, light sleep %s)",
             kPmMinFreqMhz, kPmMaxFreqMhz, kPmLightSleepEnable ? "on" : "off");
  } else {
    gPmConfigured = false;
    ESP_LOGE(kTag, "esp_pm_configure failed: 0x%x", err);
  }
#else
  ESP_LOGW(kTag, "CONFIG_PM_ENABLE is off; running without PM");
#endif
}

static void log_pm_locks() {
#if CONFIG_PM_ENABLE
  if (!gPmConfigured) {
    ESP_LOGI(kTag, "pm locks: pending (PM arms %lus after boot)",
             static_cast<unsigned long>(kPmArmDelayMs / 1000));
    return;
  }
  ESP_LOGI(kTag, "pm locks: dumping active locks");
  std::fflush(stdout);
  const esp_err_t err = esp_pm_dump_locks(stdout);
  std::fflush(stdout);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "pm locks: dump failed (err=0x%x)", err);
  }
#else
  ESP_LOGI(kTag, "pm locks: PM disabled in sdkconfig");
#endif
}

static void log_pm_status() {
#if CONFIG_PM_ENABLE
  if (!gPmConfigureAttempted) {
    ESP_LOGI(kTag, "pm: pending (arms %lus after boot)",
             static_cast<unsigned long>(kPmArmDelayMs / 1000));
    return;
  }
  if (!gPmConfigured) {
    ESP_LOGW(kTag, "pm: configure failed err=0x%x", gPmConfigureErr);
    return;
  }
  esp_pm_config_t pm = {};
  const esp_err_t err = esp_pm_get_configuration(&pm);
  if (err == ESP_OK) {
    ESP_LOGI(kTag, "pm: enabled max=%dMHz min=%dMHz light_sleep=%s",
             pm.max_freq_mhz, pm.min_freq_mhz, pm.light_sleep_enable ? "on" : "off");
  } else {
    ESP_LOGW(kTag, "pm: enabled but config unavailable (err=0x%x)", err);
  }
#else
  ESP_LOGI(kTag, "pm: disabled in sdkconfig");
#endif
}

static void metrics_set_connected(bool connected) {
  const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
  portENTER_CRITICAL(&gMetricsMux);
  if (gMetrics.connected != connected) {
    if (gMetrics.connected) {
      gMetrics.connectedUs += now - gMetrics.connStateSinceUs;
    } else {
      gMetrics.disconnectedUs += now - gMetrics.connStateSinceUs;
    }
    gMetrics.connected = connected;
    gMetrics.connStateSinceUs = now;
  }
  portEXIT_CRITICAL(&gMetricsMux);
}

static void metrics_set_advertising(bool advertising) {
  const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
  bool connectedWhileAdvertising = false;
  portENTER_CRITICAL(&gMetricsMux);
  connectedWhileAdvertising = advertising && gMetrics.connected;
  if (gMetrics.advertising != advertising) {
    if (gMetrics.advertising) {
      gMetrics.advertisingUs += now - gMetrics.advStateSinceUs;
    }
    gMetrics.advertising = advertising;
    gMetrics.advStateSinceUs = now;
  }
  portEXIT_CRITICAL(&gMetricsMux);
  if (connectedWhileAdvertising) {
    ESP_LOGW(kTag, "metrics: invariant breach attempt (advertising while connected)");
  }
}

static void log_runtime_metrics(const char* reason) {
  const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
  uint64_t uptimeUs = 0;
  uint64_t connectedUs = 0;
  uint64_t disconnectedUs = 0;
  uint64_t advertisingUs = 0;
  CpuMetrics cpu = {};
  portENTER_CRITICAL(&gMetricsMux);
  uptimeUs = now - gMetrics.bootUs;
  connectedUs = gMetrics.connectedUs + (gMetrics.connected ? (now - gMetrics.connStateSinceUs) : 0);
  disconnectedUs = gMetrics.disconnectedUs + (gMetrics.connected ? 0 : (now - gMetrics.connStateSinceUs));
  advertisingUs = gMetrics.advertisingUs + (gMetrics.advertising ? (now - gMetrics.advStateSinceUs) : 0);
  cpu = gCpuMetrics;
  portEXIT_CRITICAL(&gMetricsMux);

  const float connectedPct = uptimeUs == 0 ? 0.0f : (100.0f * static_cast<float>(connectedUs) / static_cast<float>(uptimeUs));
  const float disconnectedPct =
      uptimeUs == 0 ? 0.0f : (100.0f * static_cast<float>(disconnectedUs) / static_cast<float>(uptimeUs));
  const float advertisingPct =
      uptimeUs == 0 ? 0.0f : (100.0f * static_cast<float>(advertisingUs) / static_cast<float>(uptimeUs));

  ESP_LOGI(kTag, "metrics[%s]: up=%llus conn=%llus(%.1f%%) disc=%llus(%.1f%%) adv=%llus(%.1f%%)",
           reason,
           static_cast<unsigned long long>(uptimeUs / 1000000ULL),
           static_cast<unsigned long long>(connectedUs / 1000000ULL), connectedPct,
           static_cast<unsigned long long>(disconnectedUs / 1000000ULL), disconnectedPct,
           static_cast<unsigned long long>(advertisingUs / 1000000ULL), advertisingPct);

  const int currentMhz = esp_clk_cpu_freq() / 1000000;
  const float pct40 = cpu.samples == 0 ? 0.0f : (100.0f * static_cast<float>(cpu.mhz40) / static_cast<float>(cpu.samples));
  const float pct80 = cpu.samples == 0 ? 0.0f : (100.0f * static_cast<float>(cpu.mhz80) / static_cast<float>(cpu.samples));
  const float pct160 =
      cpu.samples == 0 ? 0.0f : (100.0f * static_cast<float>(cpu.mhz160) / static_cast<float>(cpu.samples));
  const float pct240 =
      cpu.samples == 0 ? 0.0f : (100.0f * static_cast<float>(cpu.mhz240) / static_cast<float>(cpu.samples));
  const float pctOther =
      cpu.samples == 0 ? 0.0f : (100.0f * static_cast<float>(cpu.mhzOther) / static_cast<float>(cpu.samples));
  ESP_LOGI(kTag, "metrics[%s]: cpu_freq now=%dMHz samples=%llu [40:%.1f%% 80:%.1f%% 160:%.1f%% 240:%.1f%% other:%.1f%%]",
           reason, currentMhz, static_cast<unsigned long long>(cpu.samples), pct40, pct80, pct160, pct240, pctOther);

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_USE_TRACE_FACILITY
  const UBaseType_t taskCount = uxTaskGetNumberOfTasks();
  std::vector<TaskStatus_t> taskStats(taskCount + 4);
  uint32_t totalRuntime = 0;
  const UBaseType_t populated = uxTaskGetSystemState(taskStats.data(), taskStats.size(), &totalRuntime);
  if (populated > 0 && totalRuntime > 0) {
    uint64_t idleRuntime = 0;
    for (UBaseType_t i = 0; i < populated; ++i) {
      if (std::strncmp(taskStats[i].pcTaskName, "IDLE", 4) == 0) {
        idleRuntime += taskStats[i].ulRunTimeCounter;
      }
    }
    const uint32_t cores = static_cast<uint32_t>(portNUM_PROCESSORS);
    const float capacity = static_cast<float>(totalRuntime) * static_cast<float>(cores);
    float idlePct = capacity > 0.0f ? (100.0f * static_cast<float>(idleRuntime) / capacity) : 0.0f;
    if (idlePct < 0.0f) idlePct = 0.0f;
    if (idlePct > 100.0f) idlePct = 100.0f;
    const float busyPct = 100.0f - idlePct;
    ESP_LOGI(kTag, "metrics[%s]: cpu_load busy=%.1f%% idle=%.1f%% cores=%u (task stats)",
             reason, busyPct, idlePct, static_cast<unsigned>(cores));
  } else {
    ESP_LOGW(kTag, "metrics[%s]: cpu_load unavailable (task snapshot empty)", reason);
  }
#else
  ESP_LOGI(kTag, "metrics[%s]: cpu_load unavailable (enable FREERTOS run-time stats)", reason);
#endif
}

static void pm_arm_task(void*) {
  vTaskDelay(pdMS_TO_TICKS(kPmArmDelayMs));
  configure_power_management();
  vTaskDelete(nullptr);
}

static void metrics_task(void*) {
  uint32_t elapsedMs = 0;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(kCpuSamplePeriodMs));
    cpu_metrics_sample();
    elapsedMs += kCpuSamplePeriodMs;
    if (elapsedMs >= kMetricsLogPeriodMs) {
      log_runtime_metrics("periodic");
      elapsedMs = 0;
    }
  }
}

static void log_ble_status() {
  const AdvProfileConfig advCfg = get_adv_profile_config(gAdvProfile);
  const esp_power_level_t advLevel = esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_ADV);
  const esp_power_level_t defaultLevel = esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_DEFAULT);
  ESP_LOGI(kTag, "ble: name=%s connected=%s advertising=%s interval=%.2f-%.2f s",
           kBleDeviceName,
           gBleConnHandle == BLE_HS_CONN_HANDLE_NONE ? "no" : "yes",
           gBleAdvertising ? "yes" : "no",
           advCfg.intervalMin * 0.000625f, advCfg.intervalMax * 0.000625f);
  ESP_LOGI(kTag, "ble: profile=%s duration=%s",
           advCfg.label, advCfg.durationMs == BLE_HS_FOREVER ? "forever" : "15s");
  ESP_LOGI(kTag, "ble: tx_power target=%ddBm adv=%ddBm default=%ddBm",
           ble_tx_power_dbm(gBleTxPowerTarget), ble_tx_power_dbm(advLevel), ble_tx_power_dbm(defaultLevel));
  if (gBleAddrValid) {
    ESP_LOGI(kTag, "ble: mac=%02x:%02x:%02x:%02x:%02x:%02x",
             gBleAddr[5], gBleAddr[4], gBleAddr[3], gBleAddr[2], gBleAddr[1], gBleAddr[0]);
  }
  ESP_LOGI(kTag, "ble: service=%s", kServiceUuidStr);
  ESP_LOGI(kTag, "ble: rx=%s status=%s", kRxUuidStr, kStatusUuidStr);
}

static bool handle_local_command(const std::string& raw) {
  const std::string cmd = to_lower_copy(trim_copy(raw));
  if (cmd.empty()) {
    return true;
  }
  if (cmd == "status") {
    log_status();
    return true;
  }
  if (cmd == "pm" || cmd == "pm status") {
    log_pm_status();
    return true;
  }
  if (cmd == "pm locks" || cmd == "pm lock") {
    log_pm_locks();
    return true;
  }
  if (cmd == "metrics") {
    log_runtime_metrics("manual");
    return true;
  }
  if (cmd == "txpower" || cmd == "tx power") {
    log_ble_tx_power_status();
    return true;
  }
  if (cmd.rfind("txpower ", 0) == 0 || cmd.rfind("tx power ", 0) == 0) {
    const std::string rawTrimmed = trim_copy(raw);
    const size_t prefixLen = cmd.rfind("txpower ", 0) == 0 ? 8U : 9U;
    const std::string arg = rawTrimmed.size() <= prefixLen ? "" : trim_copy(rawTrimmed.substr(prefixLen));
    esp_power_level_t level = ESP_PWR_LVL_INVALID;
    if (arg.empty() || !parse_ble_tx_power_dbm(arg, &level)) {
      ESP_LOGI(kTag, "Usage: txpower <dbm> where dbm is one of -24,-21,-18,-15,-12,-9,-6,-3,0,3,6,9,12,15,18,20");
      return true;
    }
    gBleTxPowerTarget = level;
    configure_ble_tx_power();
    log_ble_tx_power_status();
    return true;
  }
  if (cmd == "ble" || cmd == "ble status") {
    log_ble_status();
    return true;
  }
  if (cmd == "ble restart") {
    if (gBleConnHandle != BLE_HS_CONN_HANDLE_NONE) {
      ESP_LOGI(kTag, "ble: restart ignored while connected");
      return true;
    }
    const int stopRc = ble_gap_adv_stop();
    if (stopRc != 0 && stopRc != BLE_HS_EALREADY) {
      ESP_LOGW(kTag, "ble_gap_adv_stop rc=%d", stopRc);
    }
    gBleAdvertising = false;
    start_ble_advertising(AdvProfile::kFastReconnect);
    return true;
  }
  if (cmd == "help" || cmd == "?") {
    ESP_LOGI(kTag, "Commands: status | pm | pm locks | metrics | txpower [<dbm>] | ble [status|restart] | ping | reboot | send <message> | help");
    return true;
  }
  if (cmd == "ping") {
    ESP_LOGI(kTag, "PONG");
    return true;
  }
  if (cmd == "reboot" || cmd == "restart") {
    ESP_LOGW(kTag, "Reboot requested");
    esp_restart();
    return true;
  }
  return false;
}

static void process_input_line(const std::string& rawLine, InputSource source) {
  const std::string trimmed = trim_copy(rawLine);
  if (trimmed.empty()) {
    return;
  }

  const std::string lowered = to_lower_copy(trimmed);
  if (handle_local_command(trimmed)) {
    return;
  }

  if (lowered == "send") {
    ESP_LOGI(kTag, "Usage: send <message>");
    return;
  }

  if (lowered.rfind("send ", 0) == 0) {
    const std::string payload = trim_copy(trimmed.substr(4));
    if (payload.empty()) {
      ESP_LOGI(kTag, "Usage: send <message>");
    } else {
      const TickType_t waitTicks = source == InputSource::kBle ? 0 : pdMS_TO_TICKS(200);
      enqueue_message_page(payload, waitTicks);
    }
    return;
  }

  if (source == InputSource::kBle) {
    ESP_LOGW(kTag, "BLE unknown command: %s", trimmed.c_str());
  } else {
    ESP_LOGI(kTag, "Unknown command. Use: send <message>, status, pm, pm locks, metrics, txpower, ble, ping, reboot, help");
  }
}

static void process_input_payload(const std::string& payload, InputSource source) {
  size_t start = 0;
  while (start <= payload.size()) {
    const size_t end = payload.find('\n', start);
    std::string line = end == std::string::npos ? payload.substr(start)
                                                 : payload.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    process_input_line(line, source);
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
}

static void tx_worker_task(void*) {
  while (true) {
    TxJob* job = nullptr;
    if (xQueueReceive(gTxQueue, &job, portMAX_DELAY) == pdTRUE && job != nullptr) {
      bool ok = gWaveTx.transmit_bits(job->bits, gConfig);
      ESP_LOGI(kTag, "%s", ok ? "TX_DONE" : "TX_FAIL");
      delete job;
    }
  }
}

static int ble_rx_access(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  const int length = OS_MBUF_PKTLEN(ctxt->om);
  if (length <= 0) {
    return 0;
  }

  std::string payload(static_cast<size_t>(length), '\0');
  const int copyRc =
      os_mbuf_copydata(ctxt->om, 0, length, reinterpret_cast<void*>(payload.data()));
  if (copyRc != 0) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  process_input_payload(payload, InputSource::kBle);
  return 0;
}

static int ble_status_access(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  constexpr char kStatus[] = "READY";
  if (os_mbuf_append(ctxt->om, kStatus, sizeof(kStatus) - 1) != 0) {
    return BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  return 0;
}

static ble_gatt_chr_def gBleCharacteristics[] = {
    {
        .uuid = &kRxUuid.u,
        .access_cb = ble_rx_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &kStatusUuid.u,
        .access_cb = ble_status_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    },
    {
        0,
    },
};

static const ble_gatt_svc_def gBleServices[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kServiceUuid.u,
        .characteristics = gBleCharacteristics,
    },
    {
        0,
    },
};

static void ble_on_reset(int reason) { ESP_LOGW(kTag, "BLE host reset; reason=%d", reason); }

static void ble_on_sync() {
  int rc = ble_hs_id_infer_auto(0, &gBleAddrType);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_hs_id_infer_auto failed: %d", rc);
    return;
  }

  uint8_t addr[6] = {};
  rc = ble_hs_id_copy_addr(gBleAddrType, addr, nullptr);
  if (rc == 0) {
    std::memcpy(gBleAddr, addr, sizeof(addr));
    gBleAddrValid = true;
    ESP_LOGI(kTag, "BLE address %02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
  }
  configure_ble_tx_power();
  ESP_LOGI(kTag, "BLE service=%s rx=%s status=%s",
           kServiceUuidStr, kRxUuidStr, kStatusUuidStr);
  start_ble_advertising(AdvProfile::kFastReconnect);
}

static void configure_ble_tx_power() {
  const esp_err_t defErr = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, gBleTxPowerTarget);
  if (defErr != ESP_OK) {
    ESP_LOGW(kTag, "BLE tx power set default failed: 0x%x", defErr);
  }
  const esp_err_t advErr = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, gBleTxPowerTarget);
  if (advErr != ESP_OK) {
    ESP_LOGW(kTag, "BLE tx power set adv failed: 0x%x", advErr);
  }
  const esp_err_t scanErr = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, gBleTxPowerTarget);
  if (scanErr != ESP_OK) {
    ESP_LOGW(kTag, "BLE tx power set scan failed: 0x%x", scanErr);
  }
  const int advDbm = ble_tx_power_dbm(esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_ADV));
  const int defaultDbm = ble_tx_power_dbm(esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_DEFAULT));
  ESP_LOGI(kTag, "BLE tx power configured target=%ddBm adv=%ddBm default=%ddBm",
           ble_tx_power_dbm(gBleTxPowerTarget), advDbm, defaultDbm);
}

static int ble_gap_event(struct ble_gap_event* event, void*) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        gBleConnHandle = event->connect.conn_handle;
        gBleAdvertising = false;
        metrics_set_connected(true);
        metrics_set_advertising(false);
        ESP_LOGI(kTag, "BLE connected; handle=%u", static_cast<unsigned>(gBleConnHandle));
      } else {
        gBleConnHandle = BLE_HS_CONN_HANDLE_NONE;
        metrics_set_connected(false);
        ESP_LOGW(kTag, "BLE connect failed; status=%d", event->connect.status);
        start_ble_advertising(AdvProfile::kFastReconnect);
      }
      return 0;
    case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGI(kTag, "BLE disconnected; reason=%d", event->disconnect.reason);
      gBleConnHandle = BLE_HS_CONN_HANDLE_NONE;
      metrics_set_connected(false);
      start_ble_advertising(AdvProfile::kFastReconnect);
      return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      gBleAdvertising = false;
      metrics_set_advertising(false);
      if (gBleConnHandle != BLE_HS_CONN_HANDLE_NONE) {
        return 0;
      }
      if (gAdvProfile == AdvProfile::kFastReconnect &&
          event->adv_complete.reason == BLE_HS_ETIMEOUT) {
        ESP_LOGI(kTag, "BLE fast reconnect window expired; switching to slow advertising");
        start_ble_advertising(AdvProfile::kSlowIdle);
      } else {
        start_ble_advertising(gAdvProfile);
      }
      return 0;
    default:
      return 0;
  }
}

static void start_ble_advertising(AdvProfile profile) {
  if (gBleConnHandle != BLE_HS_CONN_HANDLE_NONE) {
    ESP_LOGW(kTag, "start_ble_advertising ignored while connected");
    gBleAdvertising = false;
    metrics_set_advertising(false);
    return;
  }

  const AdvProfileConfig cfg = get_adv_profile_config(profile);
  ble_hs_adv_fields advFields = {};
  advFields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  advFields.uuids128 = const_cast<ble_uuid128_t*>(&kServiceUuid);
  advFields.num_uuids128 = 1;
  advFields.uuids128_is_complete = 1;

  int rc = ble_gap_adv_set_fields(&advFields);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_gap_adv_set_fields failed: %d", rc);
    return;
  }

  ble_hs_adv_fields scanRspFields = {};
  scanRspFields.name = reinterpret_cast<const uint8_t*>(kBleDeviceName);
  scanRspFields.name_len = std::strlen(kBleDeviceName);
  scanRspFields.name_is_complete = 1;
  scanRspFields.tx_pwr_lvl_is_present = 1;
  scanRspFields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

  rc = ble_gap_adv_rsp_set_fields(&scanRspFields);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_gap_adv_rsp_set_fields failed: %d", rc);
    return;
  }

  ble_gap_adv_params params = {};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  params.itvl_min = cfg.intervalMin;
  params.itvl_max = cfg.intervalMax;

  rc = ble_gap_adv_start(gBleAddrType, nullptr, cfg.durationMs, &params, ble_gap_event, nullptr);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_gap_adv_start failed: %d", rc);
    gBleAdvertising = false;
    metrics_set_advertising(false);
    return;
  }

  gAdvProfile = profile;
  gBleAdvertising = true;
  metrics_set_advertising(true);
  ESP_LOGI(kTag, "BLE advertising (%s) as %s (interval %.2f-%.2f s, duration=%s)",
           cfg.label, kBleDeviceName, cfg.intervalMin * 0.000625f, cfg.intervalMax * 0.000625f,
           cfg.durationMs == BLE_HS_FOREVER ? "forever" : "15s");
}

static void ble_host_task(void*) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

static bool init_ble() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "nvs_flash_init failed: 0x%x", err);
    return false;
  }

  err = nimble_port_init();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "nimble_port_init failed: 0x%x", err);
    return false;
  }
  ble_hs_cfg.reset_cb = ble_on_reset;
  ble_hs_cfg.sync_cb = ble_on_sync;

  ble_svc_gap_init();
  ble_svc_gatt_init();

  int rc = 0;
  rc = ble_svc_gap_device_name_set(kBleDeviceName);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_svc_gap_device_name_set failed: %d", rc);
    return false;
  }

  rc = ble_gatts_count_cfg(gBleServices);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_gatts_count_cfg failed: %d", rc);
    return false;
  }

  rc = ble_gatts_add_svcs(gBleServices);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_gatts_add_svcs failed: %d", rc);
    return false;
  }

  nimble_port_freertos_init(ble_host_task);
  return true;
}

static void serial_input_task(void*) {
  if (!usb_serial_jtag_is_driver_installed()) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
      ESP_LOGW(kTag, "USB Serial/JTAG driver install failed; input disabled");
      vTaskDelete(nullptr);
    }
  }

  constexpr size_t kInputMax = 255;
  std::string message;
  message.reserve(kInputMax);
  bool inEscapeSequence = false;

  while (true) {
    char ch = 0;
    const int read = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(250));
    if (read <= 0) {
      continue;
    }

    if (ch == '\r') {
      continue;
    }
    if (ch == '\x1B') {
      inEscapeSequence = true;
      continue;
    }
    if (inEscapeSequence) {
      const bool done = (ch == '~') ||
                        (ch >= 'A' && ch <= 'Z') ||
                        (ch >= 'a' && ch <= 'z');
      if (done) {
        inEscapeSequence = false;
      }
      continue;
    }
    if (ch != '\n') {
      if (std::isprint(static_cast<unsigned char>(ch)) != 0 && message.size() < kInputMax) {
        message.push_back(ch);
      }
      continue;
    }

    if (message.empty()) {
      continue;
    }
    process_input_line(message, InputSource::kSerial);
    message.clear();
  }
}

extern "C" void app_main(void) {
  ESP_LOGI(kTag, "Starting ESP-IDF pager bridge");
  set_idle_line(gConfig.dataGpio, gConfig.output, gConfig.idleHigh);
  init_user_led();
  const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
  portENTER_CRITICAL(&gMetricsMux);
  gMetrics = {};
  gCpuMetrics = {};
  gMetrics.bootUs = now;
  gMetrics.connStateSinceUs = now;
  gMetrics.advStateSinceUs = now;
  gMetrics.connected = false;
  gMetrics.advertising = false;
  portEXIT_CRITICAL(&gMetricsMux);
  cpu_metrics_sample();

  gTxQueue = xQueueCreate(2, sizeof(TxJob*));
  if (gTxQueue == nullptr) {
    ESP_LOGE(kTag, "Failed to create tx queue");
    return;
  }

  xTaskCreatePinnedToCore(tx_worker_task, "tx_worker", 8192, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(serial_input_task, "serial_input", 6144, nullptr, 4, nullptr, 0);
  xTaskCreatePinnedToCore(pm_arm_task, "pm_arm", 3072, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(metrics_task, "metrics", 3072, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(user_led_task, "user_led", 2048, nullptr, 1, nullptr, 0);

  if (!init_ble()) {
    ESP_LOGE(kTag, "BLE init failed; pager bridge unavailable");
  } else {
    ESP_LOGI(kTag, "BLE ready: write 'SEND <message>' to RX characteristic");
  }
  ESP_LOGI(kTag, "PM arming in %lus; LED on GPIO%d should stay on",
           static_cast<unsigned long>(kPmArmDelayMs / 1000), kUserLedGpio);
  ESP_LOGI(kTag, "Type help for serial commands");
}
