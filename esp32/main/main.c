#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_mac.h"
#include "csi_handler.h"

// ─── Tag ────────────────────────────────────────────────────────────────────
static const char *TAG = "CSIght";

// ─── UART config ─────────────────────────────────────────────────────────────
#define UART_PORT       UART_NUM_1
#define UART_TX_PIN     GPIO_NUM_17   // default; overrideable via NVS
#define UART_RX_PIN     GPIO_NUM_16
#define UART_BAUD       115200
#define UART_BUF_SIZE   1024

// ─── Protocol bytes ──────────────────────────────────────────────────────────
#define PROTO_HANDSHAKE_REQ  0xAA
#define PROTO_HANDSHAKE_ACK  0xBB
#define PROTO_MOTION_EVT     0x01
#define PROTO_PROXIMITY_EVT  0x02
#define PROTO_WATERFALL_EVT  0x03
#define PROTO_SENSITIVITY    0x10
#define PROTO_MODE_SET       0x11

// ─── Chip model strings ───────────────────────────────────────────────────────
static const char* get_chip_name(esp_chip_model_t model) {
    switch (model) {
        case CHIP_ESP32:   return "ESP32";
        case CHIP_ESP32S2: return "ESP32-S2";
        case CHIP_ESP32S3: return "ESP32-S3";
        case CHIP_ESP32C3: return "ESP32-C3";
        case CHIP_ESP32C6: return "ESP32-C6";
        case CHIP_ESP32H2: return "ESP32-H2";
        default:           return "ESP32-??";
    }
}

// ─── CSI support level per chip ──────────────────────────────────────────────
// 0 = unsupported, 1 = limited, 2 = full
static uint8_t get_csi_support(esp_chip_model_t model) {
    switch (model) {
        case CHIP_ESP32:   return 1; // limited - older CSI API
        case CHIP_ESP32S3: return 2; // full
        case CHIP_ESP32C3: return 2; // full
        case CHIP_ESP32C6: return 2; // full
        case CHIP_ESP32S2: return 0; // no WiFi CSI
        case CHIP_ESP32H2: return 0; // 802.15.4 only, no WiFi
        default:           return 1;
    }
}

// ─── Build handshake packet ───────────────────────────────────────────────────
// Format: [0xBB][len][chip_name(null-term)][csi_support][fw_version(2B)][checksum]
static int build_handshake(uint8_t *buf, size_t buf_size) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    const char *name     = get_chip_name(chip.model);
    uint8_t     support  = get_csi_support(chip.model);
    uint8_t     fw_major = 1;
    uint8_t     fw_minor = 0;

    // name + null + support + fw_major + fw_minor
    uint8_t payload_len = (uint8_t)(strlen(name) + 1 + 1 + 1 + 1);
    if ((size_t)(payload_len + 3) > buf_size) return -1;

    int i = 0;
    buf[i++] = PROTO_HANDSHAKE_ACK;
    buf[i++] = payload_len;
    memcpy(&buf[i], name, strlen(name) + 1);
    i += strlen(name) + 1;
    buf[i++] = support;
    buf[i++] = fw_major;
    buf[i++] = fw_minor;

    // simple XOR checksum over payload
    uint8_t chk = 0;
    for (int j = 1; j < i; j++) chk ^= buf[j];
    buf[i++] = chk;

    return i;
}

// ─── UART init ───────────────────────────────────────────────────────────────
static void uart_init(void) {
    uart_config_t cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUF_SIZE * 2,
                                        UART_BUF_SIZE * 2, 0, NULL, 0));
    ESP_LOGI(TAG, "UART ready on TX=%d RX=%d @ %d baud",
             UART_TX_PIN, UART_RX_PIN, UART_BAUD);
}

// ─── WiFi init (station mode, no AP needed for CSI passive) ──────────────────
static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // set channel for passive CSI sniffing (channel 6 is densely populated)
    ESP_ERROR_CHECK(esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE));
    ESP_LOGI(TAG, "WiFi started in STA mode, channel 6");
}

// ─── UART command handler task ────────────────────────────────────────────────
static void uart_cmd_task(void *arg) {
    uint8_t rxbuf[64];
    uint8_t txbuf[64];

    while (1) {
        int len = uart_read_bytes(UART_PORT, rxbuf, sizeof(rxbuf),
                                  pdMS_TO_TICKS(20));
        if (len <= 0) continue;

        for (int i = 0; i < len; i++) {
            switch (rxbuf[i]) {

                case PROTO_HANDSHAKE_REQ: {
                    int pkt_len = build_handshake(txbuf, sizeof(txbuf));
                    if (pkt_len > 0) {
                        uart_write_bytes(UART_PORT, (const char*)txbuf, pkt_len);
                        ESP_LOGI(TAG, "Handshake sent");
                    }
                    break;
                }

                case PROTO_SENSITIVITY: {
                    // next byte is sensitivity 0-10
                    if (i + 1 < len) {
                        uint8_t sens = rxbuf[++i];
                        csi_set_sensitivity(sens);
                        ESP_LOGI(TAG, "Sensitivity set to %d", sens);
                    }
                    break;
                }

                case PROTO_MODE_SET: {
                    // next byte: 0=motion, 1=waterfall, 2=proximity
                    if (i + 1 < len) {
                        uint8_t mode = rxbuf[++i];
                        csi_set_mode(mode);
                        ESP_LOGI(TAG, "Mode set to %d", mode);
                    }
                    break;
                }

                default:
                    break;
            }
        }
    }
}

// ─── CSI event callback → sends data to Flipper ──────────────────────────────
void csight_on_motion(uint8_t intensity, uint8_t proximity) {
    uint8_t pkt[5];
    // motion packet: [0x01][intensity][proximity][chk]
    pkt[0] = PROTO_MOTION_EVT;
    pkt[1] = intensity;
    pkt[2] = proximity;
    pkt[3] = pkt[0] ^ pkt[1] ^ pkt[2];
    uart_write_bytes(UART_PORT, (const char*)pkt, 4);
}

void csight_on_waterfall(uint8_t *bins, uint8_t count) {
    // waterfall packet: [0x03][count][bin0..binN][chk]
    uint8_t pkt[64];
    if (count > 60) count = 60;
    pkt[0] = PROTO_WATERFALL_EVT;
    pkt[1] = count;
    memcpy(&pkt[2], bins, count);
    uint8_t chk = 0;
    for (int i = 0; i < count + 2; i++) chk ^= pkt[i];
    pkt[count + 2] = chk;
    uart_write_bytes(UART_PORT, (const char*)pkt, count + 3);
}

// ─── Entry point ─────────────────────────────────────────────────────────────
void app_main(void) {
    ESP_LOGI(TAG, "CSIght firmware booting...");

    // NVS init
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    uart_init();
    wifi_init();
    csi_handler_init(csight_on_motion, csight_on_waterfall);

    xTaskCreate(uart_cmd_task, "uart_cmd", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "CSIght ready. Waiting for Flipper handshake...");
}
