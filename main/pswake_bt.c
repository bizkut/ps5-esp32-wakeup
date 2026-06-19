#include "pswake_bt.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define H4_CMD 0x01
#define H4_EVT 0x04
#define EVT_CMD_COMPLETE 0x0e
#define EVT_CMD_STATUS 0x0f
#define OGF_LINK_CTL 0x01
#define OGF_INFO_PARAM 0x04
#define OCF_CREATE_CONN 0x0005
#define OCF_READ_BD_ADDR 0x0009
#define HCI_CREATE_CONN_OPCODE ((OCF_CREATE_CONN & 0x03ff) | (OGF_LINK_CTL << 10))
#define HCI_READ_BD_ADDR_OPCODE ((OCF_READ_BD_ADDR & 0x03ff) | (OGF_INFO_PARAM << 10))

static const char *TAG = "pswake_bt";
static QueueHandle_t s_evtq;
static bool s_bt_started;

typedef struct {
    uint8_t data[260];
    uint16_t len;
} hci_evt_t;

static bool parse_addr(const char *text, uint8_t out[6]) {
    if (!text) return false;

    unsigned int b[6];
    int end = 0;
    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x%n",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &end) != 6 ||
        text[end] != '\0') {
        return false;
    }
    bool all_zero = true;
    for (int i = 0; i < 6; i++) {
        if (b[i] > 0xff) return false;
        out[i] = (uint8_t)b[i];
        all_zero = all_zero && out[i] == 0;
    }
    if (all_zero) return false;
    return true;
}

static void reverse_addr(const uint8_t in[6], uint8_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = in[5 - i];
}

static void controller_ready(void) {
}

static int host_recv(uint8_t *data, uint16_t len) {
    if (!s_evtq || len > sizeof(((hci_evt_t *)0)->data)) return 0;
    hci_evt_t evt = {.len = len};
    memcpy(evt.data, data, len);
    (void)xQueueSend(s_evtq, &evt, 0);
    return 0;
}

static esp_vhci_host_callback_t s_cb = {
    .notify_host_send_available = controller_ready,
    .notify_host_recv = host_recv,
};

static esp_err_t hci_send(const uint8_t *packet, uint16_t len) {
    int waits = 0;
    while (!esp_vhci_host_check_send_available()) {
        if (++waits > 200) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    esp_vhci_host_send_packet((uint8_t *)packet, len);
    return ESP_OK;
}

static esp_err_t wait_status(uint16_t opcode, int timeout_ms, uint8_t *status_out) {
    int64_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    hci_evt_t evt;
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        TickType_t wait = deadline - xTaskGetTickCount();
        if (xQueueReceive(s_evtq, &evt, wait) != pdTRUE) break;
        if (evt.len < 6 || evt.data[0] != H4_EVT) continue;
        uint8_t event = evt.data[1];
        if (event == EVT_CMD_COMPLETE && evt.len >= 7) {
            uint16_t got = evt.data[4] | ((uint16_t)evt.data[5] << 8);
            if (got == opcode) {
                if (status_out) *status_out = evt.data[6];
                return evt.data[6] == 0 ? ESP_OK : ESP_FAIL;
            }
        } else if (event == EVT_CMD_STATUS && evt.len >= 7) {
            uint16_t got = evt.data[5] | ((uint16_t)evt.data[6] << 8);
            if (got == opcode) {
                if (status_out) *status_out = evt.data[3];
                return evt.data[3] == 0 ? ESP_OK : ESP_FAIL;
            }
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t read_bdaddr(void) {
    uint16_t opcode = HCI_READ_BD_ADDR_OPCODE;
    uint8_t cmd[] = {H4_CMD, opcode & 0xff, opcode >> 8, 0x00};
    ESP_RETURN_ON_ERROR(hci_send(cmd, sizeof(cmd)), TAG, "read bdaddr send");
    uint8_t status = 0xff;
    esp_err_t err = wait_status(opcode, 1000, &status);
    ESP_LOGI(TAG, "Read_BD_ADDR status=0x%02x err=%s", status, esp_err_to_name(err));
    return err;
}

static esp_err_t bt_start_with_controller_addr(const uint8_t controller_addr[6]) {
    if (s_bt_started) return ESP_OK;
    ESP_RETURN_ON_ERROR(esp_iface_mac_addr_set(controller_addr, ESP_MAC_BT), TAG,
                        "set BT MAC");
    uint8_t now[6] = {0};
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_read_mac(now, ESP_MAC_BT));
    ESP_LOGI(TAG, "BT MAC set to %02X:%02X:%02X:%02X:%02X:%02X",
             now[0], now[1], now[2], now[3], now[4], now[5]);

    s_evtq = xQueueCreate(16, sizeof(hci_evt_t));
    if (!s_evtq) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(esp_vhci_host_register_callback(&s_cb), TAG, "vhci cb");

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&bt_cfg), TAG, "bt init");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT), TAG,
                        "bt enable");
    s_bt_started = true;
    vTaskDelay(pdMS_TO_TICKS(300));
    return read_bdaddr();
}

esp_err_t pswake_bt_wake(const char *ps5_addr, const char *controller_addr) {
    uint8_t ps5[6], controller[6], ps5_le[6];
    ESP_RETURN_ON_FALSE(parse_addr(ps5_addr, ps5), ESP_ERR_INVALID_ARG, TAG,
                        "invalid PS5 BT address");
    ESP_RETURN_ON_FALSE(parse_addr(controller_addr, controller), ESP_ERR_INVALID_ARG,
                        TAG, "invalid controller BT address");
    ESP_RETURN_ON_ERROR(bt_start_with_controller_addr(controller), TAG, "bt start");

    reverse_addr(ps5, ps5_le);
    uint16_t opcode = HCI_CREATE_CONN_OPCODE;
    uint8_t cmd[17] = {
        H4_CMD,
        opcode & 0xff, opcode >> 8,
        13,
        ps5_le[0], ps5_le[1], ps5_le[2], ps5_le[3], ps5_le[4], ps5_le[5],
        0x18, 0xcc,
        0x02,
        0x00,
        0x00, 0x00,
        0x01,
    };

    ESP_LOGI(TAG, "Create_Connection to %s as %s", ps5_addr, controller_addr);
    ESP_RETURN_ON_ERROR(hci_send(cmd, sizeof(cmd)), TAG, "create conn send");
    uint8_t status = 0xff;
    esp_err_t err = wait_status(opcode, 3000, &status);
    ESP_LOGI(TAG, "Create_Connection status=0x%02x err=%s", status,
             esp_err_to_name(err));
    return err;
}
