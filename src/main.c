#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/rmt.h"

#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define DEVICE_NAME "PagerBridge"

#define SERVICE_UUID        "7f2b6b48-2d7e-4c35-9e5a-33e8b4e90e1b"
#define RX_CHAR_UUID        "7f2b6b49-2d7e-4c35-9e5a-33e8b4e90e1b"
#define STATUS_CHAR_UUID    "7f2b6b4a-2d7e-4c35-9e5a-33e8b4e90e1b"

#define GPIO_DATA_OUT       GPIO_NUM_4
#define DATA_OUT_IDLE_LEVEL 0
#define RMT_RESOLUTION_HZ   1000000

#define MESSAGE_QUEUE_LENGTH 5
#define MAX_MESSAGE_LEN      160

#define DEFAULT_CAPCODE 1234567
#define DEFAULT_BITRATE 512

#define TEST_PATTERN_BITS 128

#define WATCHDOG_MARGIN_US 500000

static const char *TAG = "pager_bridge";

typedef struct {
    char text[MAX_MESSAGE_LEN + 1];
} pager_message_t;

typedef struct {
    uint8_t *bits;
    size_t nbits;
} bitstream_t;

static QueueHandle_t message_queue;
static rmt_channel_t rmt_channel = RMT_CHANNEL_0;
static uint32_t current_capcode = DEFAULT_CAPCODE;
static uint16_t current_bitrate = DEFAULT_BITRATE;
static bool alert_enabled = true;

static uint16_t gatt_status_handle;
static uint16_t gatt_rx_handle;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;

static void ble_send_status(const char *status);

static void bitstream_free(bitstream_t *stream)
{
    if (stream->bits) {
        free(stream->bits);
        stream->bits = NULL;
    }
    stream->nbits = 0;
}

static esp_err_t bitstream_append(bitstream_t *stream, uint8_t bit)
{
    size_t new_bits = stream->nbits + 1;
    uint8_t *new_buf = realloc(stream->bits, new_bits);
    if (!new_buf) {
        return ESP_ERR_NO_MEM;
    }
    new_buf[stream->nbits] = bit ? 1 : 0;
    stream->bits = new_buf;
    stream->nbits = new_bits;
    return ESP_OK;
}

static esp_err_t bitstream_append_bits(bitstream_t *stream, uint32_t value, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        uint8_t bit = (value >> (count - 1 - i)) & 0x1;
        ESP_RETURN_ON_ERROR(bitstream_append(stream, bit), TAG, "append bit");
    }
    return ESP_OK;
}

static uint32_t pocsag_bch_generate(uint32_t data21)
{
    uint32_t cw = data21 << 10;
    const uint32_t poly = 0xED200;

    for (int i = 31; i >= 10; i--) {
        if (cw & (1u << i)) {
            cw ^= poly << (i - 20);
        }
    }
    return cw & 0x3FF;
}

static uint32_t pocsag_make_codeword(uint32_t data21)
{
    uint32_t bch = pocsag_bch_generate(data21);
    uint32_t codeword = (data21 << 10) | bch;
    uint32_t parity = __builtin_parity(codeword);
    return (codeword << 1) | parity;
}

static esp_err_t pocsag_append_codeword(bitstream_t *stream, uint32_t codeword)
{
    return bitstream_append_bits(stream, codeword, 32);
}

static esp_err_t pocsag_append_preamble(bitstream_t *stream)
{
    for (int i = 0; i < 576; i++) {
        ESP_RETURN_ON_ERROR(bitstream_append(stream, i % 2 == 0), TAG, "preamble");
    }
    return ESP_OK;
}

static esp_err_t pocsag_append_idle(bitstream_t *stream)
{
    return pocsag_append_codeword(stream, 0x7A89C197);
}

static esp_err_t pocsag_append_sync(bitstream_t *stream)
{
    return pocsag_append_codeword(stream, 0x7CD215D8);
}

static esp_err_t pocsag_pack_message_words(const char *text, uint32_t **words, size_t *count)
{
    size_t len = strlen(text);
    size_t max_bits = len * 7;
    size_t word_count = (max_bits + 19) / 20;
    if (word_count == 0) {
        word_count = 1;
    }

    uint32_t *out = calloc(word_count, sizeof(uint32_t));
    if (!out) {
        return ESP_ERR_NO_MEM;
    }

    size_t bit_index = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t ch = (uint8_t)text[i];
        if (ch < 0x20 || ch > 0x7f) {
            ch = '?';
        }
        uint8_t seven = ch & 0x7F;
        for (int b = 6; b >= 0; b--) {
            size_t word_idx = bit_index / 20;
            size_t bit_pos = 19 - (bit_index % 20);
            if (seven & (1u << b)) {
                out[word_idx] |= (1u << bit_pos);
            }
            bit_index++;
        }
    }

    *words = out;
    *count = word_count;
    return ESP_OK;
}

static esp_err_t encode_message_to_bitstream(const char *text, uint32_t capcode, bitstream_t *out)
{
    uint32_t *message_words = NULL;
    size_t word_count = 0;
    esp_err_t err = pocsag_pack_message_words(text, &message_words, &word_count);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t address = capcode / 8;
    uint32_t function = capcode % 8;
    uint32_t data21 = (1u << 20) | (address << 2) | (function & 0x3);
    uint32_t address_codeword = pocsag_make_codeword(data21);

    ESP_GOTO_ON_ERROR(pocsag_append_preamble(out), cleanup, TAG, "preamble");

    size_t msg_idx = 0;
    bool address_sent = false;

    while (!address_sent || msg_idx < word_count) {
        ESP_GOTO_ON_ERROR(pocsag_append_sync(out), cleanup, TAG, "sync");

        for (int frame = 0; frame < 8; frame++) {
            for (int slot = 0; slot < 2; slot++) {
                if (!address_sent && frame == (capcode & 0x7) && slot == 0) {
                    ESP_GOTO_ON_ERROR(pocsag_append_codeword(out, address_codeword), cleanup, TAG, "address");
                    address_sent = true;
                } else if (address_sent && msg_idx < word_count) {
                    uint32_t message_data = message_words[msg_idx++] & 0xFFFFF;
                    uint32_t message_codeword = pocsag_make_codeword(message_data);
                    ESP_GOTO_ON_ERROR(pocsag_append_codeword(out, message_codeword), cleanup, TAG, "message");
                } else {
                    ESP_GOTO_ON_ERROR(pocsag_append_idle(out), cleanup, TAG, "idle");
                }
            }
        }
    }

cleanup:
    free(message_words);
    return err;
}

static esp_err_t play_bits(const uint8_t *bits, size_t nbits, uint16_t bitrate)
{
    if (!bits || nbits == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t ticks = RMT_RESOLUTION_HZ / bitrate;
    if (ticks == 0 || ticks > 32767) {
        return ESP_ERR_INVALID_ARG;
    }

    rmt_item32_t *items = calloc(nbits, sizeof(rmt_item32_t));
    if (!items) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < nbits; i++) {
        items[i].level0 = bits[i] ? 1 : 0;
        items[i].duration0 = ticks;
        items[i].level1 = DATA_OUT_IDLE_LEVEL;
        items[i].duration1 = 0;
    }

    rmt_write_items(rmt_channel, items, nbits, true);
    free(items);
    return ESP_OK;
}

static void play_test_pattern(void)
{
    uint8_t pattern[TEST_PATTERN_BITS];
    for (int i = 0; i < TEST_PATTERN_BITS; i++) {
        pattern[i] = (i % 2 == 0) ? 1 : 0;
    }
    play_bits(pattern, TEST_PATTERN_BITS, current_bitrate);
}

static void set_data_idle(void)
{
    gpio_set_level(GPIO_DATA_OUT, DATA_OUT_IDLE_LEVEL);
}

static void transmit_task(void *arg)
{
    pager_message_t msg;
    while (true) {
        if (xQueueReceive(message_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        bitstream_t stream = {0};
        esp_err_t err = encode_message_to_bitstream(msg.text, current_capcode, &stream);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "encode failed: %s", esp_err_to_name(err));
            ble_send_status("ERR:ENCODE");
            bitstream_free(&stream);
            continue;
        }

        int64_t start_us = esp_timer_get_time();
        err = play_bits(stream.bits, stream.nbits, current_bitrate);
        int64_t duration_us = esp_timer_get_time() - start_us;

        if (duration_us > ((int64_t)stream.nbits * 1000000LL / current_bitrate) + WATCHDOG_MARGIN_US) {
            ESP_LOGW(TAG, "transmit watchdog tripped");
            set_data_idle();
            ble_send_status("ERR:TIMEOUT");
        } else if (err == ESP_OK) {
            ble_send_status("OK");
        } else {
            ESP_LOGE(TAG, "transmit failed: %s", esp_err_to_name(err));
            ble_send_status("ERR:TX");
        }

        bitstream_free(&stream);
    }
}

static void parse_text_payload(const char *input, char *output, size_t output_len)
{
    const char *from = strstr(input, "FROM:");
    const char *msg = strstr(input, "MSG:");

    if (from || msg) {
        char from_buf[64] = {0};
        char msg_buf[161] = {0};

        if (from) {
            from += 5;
            while (*from == ' ' || *from == '\t') {
                from++;
            }
            const char *end = strpbrk(from, "\r\n");
            size_t len = end ? (size_t)(end - from) : strlen(from);
            len = len < sizeof(from_buf) - 1 ? len : sizeof(from_buf) - 1;
            memcpy(from_buf, from, len);
        }

        if (msg) {
            msg += 4;
            while (*msg == ' ' || *msg == '\t') {
                msg++;
            }
            size_t len = strlen(msg);
            len = len < sizeof(msg_buf) - 1 ? len : sizeof(msg_buf) - 1;
            memcpy(msg_buf, msg, len);
        }

        if (strlen(from_buf) > 0 && strlen(msg_buf) > 0) {
            snprintf(output, output_len, "FROM:%s MSG:%s", from_buf, msg_buf);
        } else if (strlen(msg_buf) > 0) {
            snprintf(output, output_len, "%s", msg_buf);
        } else {
            snprintf(output, output_len, "%s", input);
        }
    } else {
        snprintf(output, output_len, "%s", input);
    }

    output[output_len - 1] = '\0';
}

static void ble_send_status(const char *status)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || gatt_status_handle == 0) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(status, strlen(status));
    if (!om) {
        return;
    }
    ble_gattc_notify_custom(conn_handle, gatt_status_handle, om);
}

static bool handle_command(const char *payload)
{
    if (strncmp(payload, "!rate=", 6) == 0) {
        uint16_t rate = (uint16_t)atoi(payload + 6);
        if (rate == 512 || rate == 1200 || rate == 2400) {
            current_bitrate = rate;
            ble_send_status("OK:RATE");
        } else {
            ble_send_status("ERR:RATE");
        }
        return true;
    }

    if (strncmp(payload, "!cap=", 5) == 0) {
        uint32_t cap = (uint32_t)strtoul(payload + 5, NULL, 10);
        if (cap > 0) {
            current_capcode = cap;
            ble_send_status("OK:CAP");
        } else {
            ble_send_status("ERR:CAP");
        }
        return true;
    }

    if (strncmp(payload, "!test=", 6) == 0) {
        int mode = atoi(payload + 6);
        if (mode == 1) {
            play_test_pattern();
            ble_send_status("OK:TEST");
        } else {
            ble_send_status("ERR:TEST");
        }
        return true;
    }

    if (strncmp(payload, "!alert=", 7) == 0) {
        int mode = atoi(payload + 7);
        alert_enabled = (mode != 0);
        ble_send_status("OK:ALERT");
        return true;
    }

    if (strncmp(payload, "!learn", 6) == 0) {
        ble_send_status("ERR:LEARN_UNSUPPORTED");
        return true;
    }

    return false;
}

static int gatt_access_cb(uint16_t conn_handle_in, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (attr_handle == gatt_rx_handle && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char payload[MAX_MESSAGE_LEN + 1] = {0};
        size_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > MAX_MESSAGE_LEN) {
            len = MAX_MESSAGE_LEN;
        }
        ble_hs_mbuf_to_flat(ctxt->om, payload, len, NULL);
        payload[len] = '\0';

        if (handle_command(payload)) {
            return 0;
        }

        pager_message_t msg = {0};
        parse_text_payload(payload, msg.text, sizeof(msg.text));

        if (xQueueSend(message_queue, &msg, 0) != pdTRUE) {
            ble_send_status("ERR:QUEUE");
        } else {
            ble_send_status("OK:QUEUED");
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID128_DECLARE(SERVICE_UUID),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID128_DECLARE(RX_CHAR_UUID),
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
                .val_handle = &gatt_rx_handle,
            },
            {
                .uuid = BLE_UUID128_DECLARE(STATUS_CHAR_UUID),
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &gatt_status_handle,
            },
            {0},
        },
    },
    {0},
};

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            conn_handle = event->connect.conn_handle;
        } else {
            conn_handle = BLE_HS_CONN_HANDLE_NONE;
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_svc_gap_device_name_set(DEVICE_NAME);
        ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                          &(struct ble_gap_adv_params){
                              .conn_mode = BLE_GAP_CONN_MODE_UND,
                              .disc_mode = BLE_GAP_DISC_MODE_GEN,
                          }, gap_event_cb, NULL);
        return 0;
    default:
        return 0;
    }
}

static void ble_app_on_sync(void)
{
    ble_hs_id_infer_auto(0, NULL);

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    fields.name = (const uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;

    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };

    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t init_rmt(void)
{
    rmt_config_t rmt_config = {
        .rmt_mode = RMT_MODE_TX,
        .channel = rmt_channel,
        .gpio_num = GPIO_DATA_OUT,
        .mem_block_num = 1,
        .clk_div = 80,  // 1 MHz resolution
        .tx_config = {
            .loop_en = false,
            .carrier_freq_hz = 0,
            .carrier_level = RMT_CARRIER_LEVEL_LOW,
            .carrier_en = false,
            .idle_level = DATA_OUT_IDLE_LEVEL,
            .idle_output_en = true,
        }
    };
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    
    ESP_RETURN_ON_ERROR(rmt_config(&rmt_config), TAG, "rmt config");
    ESP_RETURN_ON_ERROR(rmt_driver_install(rmt_channel, 0, 0), TAG, "rmt driver install");
    return ESP_OK;
}

static void configure_gpio(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_DATA_OUT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = 0,
        .pull_up_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    set_data_idle();
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_nimble_hci_and_controller_init());
    nimble_port_init();

    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);

    ble_hs_cfg.sync_cb = ble_app_on_sync;

    message_queue = xQueueCreate(MESSAGE_QUEUE_LENGTH, sizeof(pager_message_t));
    if (!message_queue) {
        ESP_LOGE(TAG, "failed to create message queue");
        return;
    }

    configure_gpio();
    ESP_ERROR_CHECK(init_rmt());

    xTaskCreate(transmit_task, "pager_tx", 4096, NULL, 5, NULL);

    nimble_port_freertos_init(ble_host_task);
}
