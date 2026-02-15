#include <cstdint>
#include <string>
#include <vector>

#include "driver/gpio.h"
#include "driver/rmt.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {
constexpr char kTag[] = "pocsag_tx";
constexpr uint32_t kSyncWord = 0x7CD215D8;
constexpr uint32_t kIdleWord = 0x7A89C197;
constexpr uint32_t kMaxRmtDuration = 32767;
constexpr size_t kMaxRmtItems = 2000;
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

    const esp_err_t err = rmt_write_items(channel_, items_.data(), items_.size(), true);
    if (err != ESP_OK) {
      ESP_LOGE(kTag, "rmt_write_items failed: 0x%x", err);
    }

    rmt_tx_stop(channel_);
    rmt_driver_uninstall(channel_);
    initialized_ = false;
    set_idle_line(cfg.dataGpio, cfg.output, cfg.idleHigh);
    busy_ = false;
    return err == ESP_OK;
  }

 private:
  bool ensure_rmt(int gpio, OutputMode output, bool idleHigh) {
    if (initialized_) {
      rmt_driver_uninstall(channel_);
      initialized_ = false;
    }

    set_idle_line(gpio, output, idleHigh);
    rmt_config_t rmt_cfg = {};
    rmt_cfg.rmt_mode = RMT_MODE_TX;
    rmt_cfg.channel = channel_;
    rmt_cfg.gpio_num = static_cast<gpio_num_t>(gpio);
    rmt_cfg.mem_block_num = 4;
    rmt_cfg.clk_div = 80;
    rmt_cfg.tx_config.loop_en = false;
    rmt_cfg.tx_config.carrier_en = false;
    rmt_cfg.tx_config.idle_output_en = true;
    rmt_cfg.tx_config.idle_level = idleHigh ? RMT_IDLE_LEVEL_HIGH : RMT_IDLE_LEVEL_LOW;

    esp_err_t err = rmt_config(&rmt_cfg);
    if (err != ESP_OK) {
      ESP_LOGE(kTag, "rmt_config failed: 0x%x", err);
      return false;
    }

    err = rmt_driver_install(channel_, 0, 0);
    if (err != ESP_OK) {
      ESP_LOGE(kTag, "rmt_driver_install failed: 0x%x", err);
      return false;
    }

    initialized_ = true;
    return true;
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
        rmt_item32_t item = {};
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

  rmt_channel_t channel_ = RMT_CHANNEL_0;
  bool initialized_ = false;
  bool busy_ = false;
  std::vector<rmt_item32_t> items_;
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

static void tx_worker_task(void*) {
  while (true) {
    TxJob* job = nullptr;
    if (xQueueReceive(gTxQueue, &job, portMAX_DELAY) == pdTRUE && job != nullptr) {
      bool ok = gWaveTx.transmit_bits(job->bits, gConfig);
      ESP_LOGI(kTag, ok ? "TX_DONE" : "TX_FAIL");
      delete job;
    }
  }
}

extern "C" void app_main(void) {
  ESP_LOGI(kTag, "Starting ESP-IDF POCSAG baseline (no power management)");
  set_idle_line(gConfig.dataGpio, gConfig.output, gConfig.idleHigh);

  gTxQueue = xQueueCreate(2, sizeof(TxJob*));
  if (gTxQueue == nullptr) {
    ESP_LOGE(kTag, "Failed to create tx queue");
    return;
  }

  xTaskCreatePinnedToCore(tx_worker_task, "tx_worker", 8192, nullptr, 5, nullptr, 0);

  const std::vector<uint8_t> bits = build_pocsag_bits("HELLO WORLD", gConfig);
  TxJob* bootJob = new TxJob{bits};
  if (xQueueSend(gTxQueue, &bootJob, 0) != pdTRUE) {
    delete bootJob;
    ESP_LOGW(kTag, "Queue busy; boot message not sent");
  }
}
