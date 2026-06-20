#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"
#include "pswake_bt.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define MANUAL_WAKE_DONE_MS 5000

typedef enum {
    ST_IDLE,
    ST_ARMED,
    ST_REST_DETECTED,
    ST_WAKE_SENT,
    ST_WAIT_LINUX,
    ST_LINUX_ON,
    ST_ERROR,
} app_state_t;

typedef struct {
    char name[32];
    char token[64];
    char ps5_bt_addr[18];
    char controller_bt_addr[18];
    char last_ip[16];
    bool enabled;
} profile_t;

static const char *TAG = "pswake";
static EventGroupHandle_t s_wifi_events;
static int s_wifi_retries;
static profile_t s_profile;
static app_state_t s_state = ST_IDLE;
static char s_state_line[64];
static char s_error[64];
static int s_wake_attempts;
static char s_wifi_status[24];
static volatile bool s_manual_wake_active;
static volatile bool s_bt_wake_active;

static void wake_flow_task(void *arg);
static void manual_bt_wake_task(void *arg);

static void ping_success_cb(esp_ping_handle_t hdl, void *args) {
    (void)hdl;
    bool *result = (bool *)args;
    *result = true;
}

static const char *state_name(app_state_t st) {
    switch (st) {
    case ST_IDLE: return "IDLE";
    case ST_ARMED: return "ARMED";
    case ST_REST_DETECTED: return "REST";
    case ST_WAKE_SENT: return "WAKE";
    case ST_WAIT_LINUX: return "WAIT LINUX";
    case ST_LINUX_ON: return "LINUX ON";
    case ST_ERROR: return "ERROR";
    default: return "?";
    }
}

static void set_state(app_state_t st, const char *line, const char *err) {
    if ((line && strcmp(line, "LINUX BOOT") == 0) || st == ST_LINUX_ON) {
        display_set_os(DISPLAY_OS_LINUX);
    }
    s_state = st;
    if (line) strlcpy(s_state_line, line, sizeof(s_state_line));
    if (err) {
        strlcpy(s_error, err, sizeof(s_error));
    } else {
        s_error[0] = 0;
    }
    display_set_net_status(s_wifi_status);
    char l2[64];
    snprintf(l2, sizeof(l2), "%s %s", s_profile.name, s_profile.last_ip);
    display_status(state_name(st), s_state_line, l2, s_error);
    ESP_LOGI(TAG, "state=%s %s %s", state_name(st), s_state_line, s_error);
}

static bool wake_flow_cancelled(void) {
    return s_manual_wake_active || s_state == ST_IDLE || s_state == ST_LINUX_ON;
}

static void start_wake_flow(const char *ip, const char *task_name) {
    char *task_ip = strdup(ip);
    if (!task_ip) {
        set_state(ST_ERROR, "TASK FAILED", "NO MEM");
        return;
    }

    BaseType_t ok = xTaskCreate(wake_flow_task, task_name, 6144, task_ip, 5, NULL);
    if (ok != pdPASS) {
        free(task_ip);
        set_state(ST_ERROR, "TASK FAILED", "NO TASK");
    }
}

static void start_manual_bt_wake(void) {
    if (s_manual_wake_active || s_bt_wake_active) return;
    s_manual_wake_active = true;

    BaseType_t ok = xTaskCreate(manual_bt_wake_task, "bt_wake_manual", 6144,
                                NULL, 5, NULL);
    if (ok != pdPASS) {
        s_manual_wake_active = false;
        set_state(ST_ERROR, "TASK FAILED", "NO TASK");
    }
}

static uint64_t gpio_pin_mask_if_valid(int gpio) {
    if (gpio < 0 || gpio > 39) return 0;
    return 1ULL << gpio;
}

static void load_profile(void) {
    memset(&s_profile, 0, sizeof(s_profile));
    strlcpy(s_profile.name, CONFIG_PSWAKE_PROFILE_NAME, sizeof(s_profile.name));
    strlcpy(s_profile.token, CONFIG_PSWAKE_TOKEN, sizeof(s_profile.token));
    strlcpy(s_profile.ps5_bt_addr, CONFIG_PSWAKE_PS5_BT_ADDR,
            sizeof(s_profile.ps5_bt_addr));
    strlcpy(s_profile.controller_bt_addr, CONFIG_PSWAKE_CONTROLLER_BT_ADDR,
            sizeof(s_profile.controller_bt_addr));
    s_profile.enabled = true;

    nvs_handle_t nvs;
    if (nvs_open("pswake", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_profile.last_ip);
        nvs_get_str(nvs, "last_ip", s_profile.last_ip, &len);
        nvs_close(nvs);
    }
}

static void save_last_ip(const char *ip) {
    strlcpy(s_profile.last_ip, ip, sizeof(s_profile.last_ip));
    nvs_handle_t nvs;
    if (nvs_open("pswake", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "last_ip", ip);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retries++ < 10) {
            strlcpy(s_wifi_status, "STA ...", sizeof(s_wifi_status));
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        s_wifi_retries = 0;
        snprintf(s_wifi_status, sizeof(s_wifi_status), "STA " IPSTR,
                 IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t start_ap(void) {
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init ap");
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, CONFIG_PSWAKE_AP_SSID, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, CONFIG_PSWAKE_AP_PASSWORD,
            sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(CONFIG_PSWAKE_AP_SSID);
    ap.ap.max_connection = 4;
    ap.ap.authmode = strlen(CONFIG_PSWAKE_AP_PASSWORD) ? WIFI_AUTH_WPA_WPA2_PSK
                                                       : WIFI_AUTH_OPEN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "wifi ap mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "wifi ap cfg");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi ap start");
    strlcpy(s_wifi_status, "AP 192.168.4.1", sizeof(s_wifi_status));
    set_state(ST_IDLE, "AP 192.168.4.1", NULL);
    return ESP_OK;
}

static esp_err_t start_wifi(void) {
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_wifi_events = xEventGroupCreate();
    strlcpy(s_wifi_status, "NET ...", sizeof(s_wifi_status));

    if (strlen(CONFIG_PSWAKE_WIFI_SSID) == 0) {
#if CONFIG_PSWAKE_AP_FALLBACK
        return start_ap();
#else
        return ESP_ERR_INVALID_STATE;
#endif
    }

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event, NULL));

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, CONFIG_PSWAKE_WIFI_SSID, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, CONFIG_PSWAKE_WIFI_PASSWORD,
            sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "wifi cfg");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT) {
        set_state(ST_IDLE, "WIFI READY", NULL);
        return ESP_OK;
    }
#if CONFIG_PSWAKE_AP_FALLBACK
    esp_wifi_stop();
    esp_wifi_deinit();
    return start_ap();
#else
    return ESP_ERR_TIMEOUT;
#endif
}

static bool ping_once(ip_addr_t target) {
    bool ok = false;
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = 1;
    cfg.interval_ms = 1000;
    cfg.timeout_ms = 1000;
    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_success_cb,
        .cb_args = &ok,
    };
    esp_ping_handle_t hdl = NULL;
    if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK) return false;
    esp_ping_start(hdl);
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_ping_delete_session(hdl);
    return ok;
}

static bool token_matches(const char *payload, int len, bool *linux_up) {
    char msg[192];
    int copy = len < (int)sizeof(msg) - 1 ? len : (int)sizeof(msg) - 1;
    memcpy(msg, payload, copy);
    msg[copy] = 0;
    *linux_up = strstr(msg, "PS5LINUX_UP v1") != NULL;
    if (!strstr(msg, "PS5LINUX_ARMED v1") && !*linux_up) return false;
    const char *tok = strstr(msg, "token=");
    if (!tok) return false;
    tok += 6;
    size_t want = strlen(s_profile.token);
    return strncmp(tok, s_profile.token, want) == 0 &&
           (tok[want] == 0 || tok[want] == ' ' || tok[want] == '\n' ||
            tok[want] == '\r');
}

static void wake_flow_task(void *arg) {
    char ip[16];
    strlcpy(ip, (char *)arg, sizeof(ip));
    free(arg);

    ip_addr_t target;
    if (!ipaddr_aton(ip, &target)) {
        set_state(ST_ERROR, "BAD IP", ip);
        vTaskDelete(NULL);
        return;
    }
    display_set_os(DISPLAY_OS_PS5);
    set_state(ST_ARMED, "LINUX TIME", NULL);

    int down_ms = 0;
    bool rest_pending = false;
    while (down_ms < CONFIG_PSWAKE_PING_TIMEOUT_MS) {
        if (wake_flow_cancelled()) {
            vTaskDelete(NULL);
            return;
        }
        bool ok = ping_once(target);
        if (!ok || rest_pending) {
            rest_pending = true;
            down_ms += CONFIG_PSWAKE_PING_INTERVAL_MS;
        }
        strlcpy(s_state_line, rest_pending ? "WAIT REST" : "LINUX TIME",
                sizeof(s_state_line));
        set_state(ST_ARMED, s_state_line, NULL);
        if (down_ms >= CONFIG_PSWAKE_PING_TIMEOUT_MS) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_PSWAKE_PING_INTERVAL_MS));
    }

    set_state(ST_REST_DETECTED, "WAIT REST", NULL);
    if (CONFIG_PSWAKE_REST_SETTLE_MS > 0) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_PSWAKE_REST_SETTLE_MS));
    }

    while (!wake_flow_cancelled()) {
        if (ping_once(target)) {
            break;
        }
        set_state(ST_REST_DETECTED, "WAIT REST", NULL);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_PSWAKE_PING_INTERVAL_MS));
    }
    if (wake_flow_cancelled()) {
        vTaskDelete(NULL);
        return;
    }

    bool wake_ok = false;
    for (int i = 0; i < CONFIG_PSWAKE_WAKE_RETRIES; i++) {
        if (wake_flow_cancelled()) {
            vTaskDelete(NULL);
            return;
        }
        s_wake_attempts++;
        set_state(ST_WAKE_SENT, "WAKING UP", NULL);
        s_bt_wake_active = true;
        esp_err_t err = pswake_bt_wake(s_profile.ps5_bt_addr,
                                       s_profile.controller_bt_addr);
        s_bt_wake_active = false;
        if (err == ESP_OK) {
            wake_ok = true;
            break;
        }
        snprintf(s_error, sizeof(s_error), "BT %s", esp_err_to_name(err));
        set_state(ST_ERROR, "WAKE FAILED", s_error);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (!wake_ok) {
        set_state(ST_ERROR, "WAKE FAILED", s_error[0] ? s_error : "NO RETRY");
        vTaskDelete(NULL);
        return;
    }

    set_state(ST_WAIT_LINUX, "WAKING UP", NULL);
    int waited = 0;
    bool awake_seen = false;
    bool linux_boot_seen = false;
    int boot_down_ms = 0;
    while (waited < CONFIG_PSWAKE_LINUX_UP_TIMEOUT_MS && s_state == ST_WAIT_LINUX) {
        bool ok = ping_once(target);
        if (!awake_seen) {
            awake_seen = ok;
        } else if (ok) {
            boot_down_ms = 0;
        } else {
            boot_down_ms += CONFIG_PSWAKE_PING_INTERVAL_MS;
            if (boot_down_ms >= CONFIG_PSWAKE_PING_TIMEOUT_MS) {
                linux_boot_seen = true;
                display_set_os(DISPLAY_OS_LINUX);
                set_state(ST_WAIT_LINUX, "LINUX BOOT", NULL);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        waited += 2200;
    }
    while (linux_boot_seen && waited < CONFIG_PSWAKE_LINUX_UP_TIMEOUT_MS &&
           s_state == ST_WAIT_LINUX) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        waited += 1000;
    }
    if (s_state == ST_WAIT_LINUX) {
        display_set_os(DISPLAY_OS_PS5);
        set_state(ST_IDLE, "READY",
                  awake_seen ? "LINUX UP TIMEOUT" : "BOOT TIMEOUT");
    }
    vTaskDelete(NULL);
}

static void manual_bt_wake_task(void *arg) {
    (void)arg;
    display_set_os(DISPLAY_OS_PS5);
    s_wake_attempts++;
    set_state(ST_WAKE_SENT, "WAKING UP", NULL);

    s_bt_wake_active = true;
    esp_err_t err = pswake_bt_wake(s_profile.ps5_bt_addr,
                                   s_profile.controller_bt_addr);
    s_bt_wake_active = false;
    if (err != ESP_OK) {
        snprintf(s_error, sizeof(s_error), "BT %s", esp_err_to_name(err));
        set_state(ST_ERROR, "WAKE FAILED", s_error);
        s_manual_wake_active = false;
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(MANUAL_WAKE_DONE_MS));
    if (s_state == ST_WAKE_SENT) {
        set_state(ST_IDLE, "READY", "WAKE SENT");
    }
    s_manual_wake_active = false;
    vTaskDelete(NULL);
}

static void udp_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        set_state(ST_ERROR, "UDP SOCKET", strerror(errno));
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in listen_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_PSWAKE_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        set_state(ST_ERROR, "UDP BIND", strerror(errno));
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    set_state(ST_IDLE, "READY", NULL);
    while (1) {
        char rx[192];
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int len = recvfrom(sock, rx, sizeof(rx) - 1, 0, (struct sockaddr *)&src, &slen);
        if (len <= 0) continue;
        bool linux_up = false;
        if (!token_matches(rx, len, &linux_up)) continue;
        char ip[16];
        inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
        save_last_ip(ip);
        if (linux_up) {
            display_set_os(DISPLAY_OS_LINUX);
            set_state(ST_LINUX_ON, "BOOT CONFIRMED", NULL);
            continue;
        }
        if (s_state == ST_IDLE || s_state == ST_ERROR || s_state == ST_LINUX_ON) {
            display_set_os(DISPLAY_OS_PS5);
            start_wake_flow(ip, "wake_flow");
        }
    }
}

static void button_task(void *arg) {
    uint64_t pin_mask = gpio_pin_mask_if_valid(CONFIG_PSWAKE_BTN_WAKE_GPIO) |
                        gpio_pin_mask_if_valid(CONFIG_PSWAKE_BTN_CANCEL_GPIO);
    if (!pin_mask) {
        ESP_LOGI(TAG, "buttons disabled by device profile");
        vTaskDelete(NULL);
        return;
    }

    gpio_config_t io = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    while (1) {
        if (CONFIG_PSWAKE_BTN_WAKE_GPIO >= 0 &&
            gpio_get_level(CONFIG_PSWAKE_BTN_WAKE_GPIO) == 0) {
            start_manual_bt_wake();
            vTaskDelay(pdMS_TO_TICKS(700));
        }
        if (CONFIG_PSWAKE_BTN_CANCEL_GPIO >= 0 &&
            gpio_get_level(CONFIG_PSWAKE_BTN_CANCEL_GPIO) == 0) {
            display_set_os(DISPLAY_OS_PS5);
            set_state(ST_IDLE, "CANCELLED", NULL);
            vTaskDelay(pdMS_TO_TICKS(700));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    load_profile();
    ESP_ERROR_CHECK_WITHOUT_ABORT(display_init());
    display_set_os(DISPLAY_OS_PS5);
    set_state(ST_IDLE, "BOOT", NULL);
    ESP_ERROR_CHECK_WITHOUT_ABORT(start_wifi());
    xTaskCreate(udp_task, "udp", 4096, NULL, 5, NULL);
    xTaskCreate(button_task, "buttons", 2048, NULL, 4, NULL);
}
