#include "telegram_notifier.h"

#include <inttypes.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "tg_notifier";
static const char *TG_NVS_NS = "zb_probe";
static const char *TG_NVS_KEY = "telegram";
static const uint32_t TG_CFG_MAGIC = 0x54474E46UL;
static const EventBits_t WIFI_CONNECTED_BIT = BIT0;
#define WIFI_SCAN_MAX_AP 20U
static const size_t WIFI_SSID_MAX_LEN = 32;
static const size_t WIFI_PASSWORD_MAX_LEN = 64;
static const size_t TG_TOKEN_MAX_LEN = 128;
static const size_t TG_CHAT_ID_MAX_LEN = 64;

typedef struct {
    uint32_t magic;
    char wifi_ssid[33];
    char wifi_password[65];
    char bot_token[129];
    char chat_id[65];
} telegram_cfg_store_t;

typedef struct {
    char text[192];
} telegram_job_t;

static telegram_cfg_store_t s_cfg;
static bool s_cfg_loaded;
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_netif_initialized;
static bool s_event_loop_ready;
static bool s_wifi_connected;
static bool s_wifi_keep_connected;
static char s_last_send_status[96];
static EventGroupHandle_t s_event_group;
static QueueHandle_t s_job_queue;
static SemaphoreHandle_t s_lock;
static wifi_ap_record_t s_scan_results[WIFI_SCAN_MAX_AP];
static uint16_t s_scan_result_count;

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0U) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_size, "%s", src);
}

static bool config_complete_locked(void)
{
    return s_cfg.wifi_ssid[0] != '\0' &&
           s_cfg.wifi_password[0] != '\0' &&
           s_cfg.bot_token[0] != '\0' &&
           s_cfg.chat_id[0] != '\0';
}

static bool wifi_config_present_locked(void)
{
    return s_cfg.wifi_ssid[0] != '\0' && s_cfg.wifi_password[0] != '\0';
}

static void set_last_status_locked(const char *status)
{
    copy_string(s_last_send_status, sizeof(s_last_send_status), status);
}

static void load_config(void)
{
    nvs_handle_t nvs = 0;
    telegram_cfg_store_t cfg = {0};
    size_t size = sizeof(cfg);
    esp_err_t err = nvs_open(TG_NVS_NS, NVS_READONLY, &nvs);

    if (err != ESP_OK) {
        return;
    }

    err = nvs_get_blob(nvs, TG_NVS_KEY, &cfg, &size);
    nvs_close(nvs);
    if (err != ESP_OK || size != sizeof(cfg) || cfg.magic != TG_CFG_MAGIC) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = cfg;
    s_cfg_loaded = true;
    xSemaphoreGive(s_lock);
}

static esp_err_t save_config_locked(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err;

    s_cfg.magic = TG_CFG_MAGIC;
    err = nvs_open(TG_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(nvs, TG_NVS_KEY, &s_cfg, sizeof(s_cfg));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        s_cfg_loaded = true;
    }
    return err;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    bool should_connect = false;

    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        should_connect = s_wifi_keep_connected && wifi_config_present_locked();
        xSemaphoreGive(s_lock);
        if (should_connect) {
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_wifi_connected = false;
        should_connect = s_wifi_keep_connected && wifi_config_present_locked();
        xSemaphoreGive(s_lock);
        if (s_event_group) {
            xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
        }
        if (should_connect) {
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_wifi_connected = true;
        xSemaphoreGive(s_lock);
        if (s_event_group) {
            xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

static esp_err_t apply_wifi_config_locked(void)
{
    wifi_config_t wifi_cfg = {0};
    esp_err_t err;

    if (s_cfg.wifi_ssid[0] == '\0' || s_cfg.wifi_password[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    copy_string((char *)wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), s_cfg.wifi_ssid);
    copy_string((char *)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), s_cfg.wifi_password);
    wifi_cfg.sta.scan_method = WIFI_FAST_SCAN;
    wifi_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to set Wi-Fi config: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Unable to start Wi-Fi: %s", esp_err_to_name(err));
            return err;
        }
        s_wifi_started = true;
    } else {
        esp_wifi_disconnect();
    }

    if (s_wifi_connected) {
        return ESP_OK;
    }

    return esp_wifi_connect();
}

static esp_err_t ensure_wifi_driver_ready(void)
{
    esp_err_t err;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_netif_initialized) {
        err = esp_netif_init();
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            s_netif_initialized = true;
        } else {
            xSemaphoreGive(s_lock);
            return err;
        }
    }

    if (!s_event_loop_ready) {
        err = esp_event_loop_create_default();
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            s_event_loop_ready = true;
        } else {
            xSemaphoreGive(s_lock);
            return err;
        }
    }

    if (!s_wifi_initialized) {
        wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();

        esp_netif_create_default_wifi_sta();
        err = esp_wifi_init(&wifi_init_cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Unable to init Wi-Fi: %s", esp_err_to_name(err));
            xSemaphoreGive(s_lock);
            return err;
        }

        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                         &wifi_event_handler, NULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Unable to register Wi-Fi handler: %s", esp_err_to_name(err));
            xSemaphoreGive(s_lock);
            return err;
        }

        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                         &wifi_event_handler, NULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Unable to register IP handler: %s", esp_err_to_name(err));
            xSemaphoreGive(s_lock);
            return err;
        }

        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Unable to set Wi-Fi mode: %s", esp_err_to_name(err));
            xSemaphoreGive(s_lock);
            return err;
        }
        s_wifi_initialized = true;
    }

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static esp_err_t ensure_wifi_ready(void)
{
    esp_err_t err;

    err = ensure_wifi_driver_ready();
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_wifi_keep_connected = true;
    err = apply_wifi_config_locked();
    if (err != ESP_OK) {
        s_wifi_keep_connected = false;
    }
    xSemaphoreGive(s_lock);
    return err;
}

static void stop_wifi_session(void)
{
    bool should_disconnect = false;

    if (!s_lock) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_wifi_keep_connected = false;
    s_wifi_connected = false;
    should_disconnect = s_wifi_started;
    xSemaphoreGive(s_lock);

    if (s_event_group) {
        xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
    }

    if (!should_disconnect) {
        return;
    }

    esp_wifi_disconnect();
}

static bool time_is_valid(void)
{
    time_t now = time(NULL);
    return now >= 1700000000;
}

static void format_timestamp(char *buffer, size_t buffer_size)
{
    time_t now;
    struct tm tm_info = {0};

    if (time_is_valid()) {
        now = time(NULL);
        gmtime_r(&now, &tm_info);
        strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S UTC", &tm_info);
        return;
    }

    snprintf(buffer, buffer_size, "t=%" PRIu64 "ms", esp_timer_get_time() / 1000ULL);
}

static char *url_encode(const char *input)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t in_len;
    size_t out_len = 0;
    char *output;
    size_t j = 0;

    if (!input) {
        return NULL;
    }

    in_len = strlen(input);
    for (size_t i = 0; i < in_len; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out_len += 1;
        } else {
            out_len += 3;
        }
    }

    output = calloc(out_len + 1, 1);
    if (!output) {
        return NULL;
    }

    for (size_t i = 0; i < in_len; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            output[j++] = (char)c;
        } else {
            output[j++] = '%';
            output[j++] = hex[c >> 4];
            output[j++] = hex[c & 0x0F];
        }
    }
    output[j] = '\0';
    return output;
}

static esp_err_t telegram_send_text(const char *text)
{
    esp_http_client_config_t http_cfg = {0};
    esp_http_client_handle_t client = NULL;
    char url[256];
    char *encoded_chat = NULL;
    char *encoded_text = NULL;
    char *body = NULL;
    int status_code = -1;
    esp_err_t err = ESP_FAIL;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!config_complete_locked()) {
        set_last_status_locked("missing config");
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", s_cfg.bot_token);
    encoded_chat = url_encode(s_cfg.chat_id);
    xSemaphoreGive(s_lock);
    encoded_text = url_encode(text);

    if (!encoded_chat || !encoded_text) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    body = calloc(strlen(encoded_chat) + strlen(encoded_text) + 32, 1);
    if (!body) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    snprintf(body, strlen(encoded_chat) + strlen(encoded_text) + 32,
             "chat_id=%s&text=%s", encoded_chat, encoded_text);

    http_cfg.url = url;
    http_cfg.method = HTTP_METHOD_POST;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.timeout_ms = 10000;

    client = esp_http_client_init(&http_cfg);
    if (!client) {
        err = ESP_FAIL;
        goto cleanup;
    }

    err = esp_http_client_set_header(client, "Content-Type",
                                     "application/x-www-form-urlencoded");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to set Telegram header: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = esp_http_client_set_post_field(client, body, strlen(body));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to set Telegram body: %s", esp_err_to_name(err));
        goto cleanup;
    }
    err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        goto cleanup;
    }

    status_code = esp_http_client_get_status_code(client);
    if (status_code < 200 || status_code >= 300) {
        err = ESP_FAIL;
        goto cleanup;
    }

cleanup:
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (err == ESP_OK) {
        set_last_status_locked("last send ok");
    } else if (status_code > 0) {
        snprintf(s_last_send_status, sizeof(s_last_send_status), "http %d", status_code);
    } else {
        snprintf(s_last_send_status, sizeof(s_last_send_status), "%s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);

    if (client) {
        esp_http_client_cleanup(client);
    }
    free(body);
    free(encoded_chat);
    free(encoded_text);
    return err;
}

static void telegram_worker_task(void *arg)
{
    telegram_job_t job;

    (void)arg;
    for (;;) {
        if (xQueueReceive(s_job_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (ensure_wifi_ready() != ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            set_last_status_locked("wifi init failed");
            xSemaphoreGive(s_lock);
            stop_wifi_session();
            continue;
        }

        if (xEventGroupWaitBits(s_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                pdMS_TO_TICKS(15000)) == 0) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            set_last_status_locked("wifi timeout");
            xSemaphoreGive(s_lock);
            stop_wifi_session();
            continue;
        }

        telegram_send_text(job.text);
        stop_wifi_session();
    }
}

void telegram_notifier_init(void)
{
    if (s_lock) {
        return;
    }

    s_lock = xSemaphoreCreateMutex();
    s_event_group = xEventGroupCreate();
    s_job_queue = xQueueCreate(8, sizeof(telegram_job_t));
    if (!s_lock || !s_event_group || !s_job_queue) {
        ESP_LOGE(TAG, "Unable to initialize Telegram notifier primitives");
        return;
    }

    s_last_send_status[0] = '\0';
    load_config();
    if (xTaskCreate(telegram_worker_task, "telegram_worker", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Unable to start Telegram worker task");
        return;
    }

    ESP_LOGI(TAG, "Telegram notifier ready");
}

void telegram_notifier_notify_presence(bool occupied)
{
    telegram_job_t job = {0};
    char timestamp[48];

    if (!s_job_queue || !s_lock) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!config_complete_locked()) {
        xSemaphoreGive(s_lock);
        return;
    }
    xSemaphoreGive(s_lock);

    format_timestamp(timestamp, sizeof(timestamp));
    snprintf(job.text, sizeof(job.text), "%s %s", occupied ? "PRESENT" : "CLEAR", timestamp);
    if (xQueueSend(s_job_queue, &job, 0) != pdTRUE) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        set_last_status_locked("queue full");
        xSemaphoreGive(s_lock);
    }
}

esp_err_t telegram_notifier_set_wifi(const char *ssid, const char *password)
{
    esp_err_t err;

    if (!ssid || !password || ssid[0] == '\0' || password[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) > WIFI_SSID_MAX_LEN || strlen(password) > WIFI_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy_string(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), ssid);
    copy_string(s_cfg.wifi_password, sizeof(s_cfg.wifi_password), password);
    err = save_config_locked();
    if (err == ESP_OK) {
        set_last_status_locked("wifi config saved");
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        return err;
    }
    stop_wifi_session();
    return ESP_OK;
}

esp_err_t telegram_notifier_scan_wifi(void)
{
    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
    };
    wifi_config_t empty_cfg = {0};
    uint16_t ap_count = WIFI_SCAN_MAX_AP;
    esp_err_t err = ensure_wifi_driver_ready();

    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err == ESP_OK) {
            s_wifi_started = true;
        } else if (err != ESP_ERR_INVALID_STATE) {
            xSemaphoreGive(s_lock);
            return err;
        }
    }
    xSemaphoreGive(s_lock);

    err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &empty_cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        stop_wifi_session();
        return err;
    }

    memset(s_scan_results, 0, sizeof(s_scan_results));
    err = esp_wifi_scan_get_ap_records(&ap_count, s_scan_results);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_scan_result_count = ap_count;
    set_last_status_locked(ap_count > 0 ? "wifi scan ok" : "wifi scan empty");
    xSemaphoreGive(s_lock);

    printf("wifi_scan_count=%u\n", ap_count);
    for (uint16_t i = 0; i < ap_count; ++i) {
        const wifi_ap_record_t *ap = &s_scan_results[i];
        printf("[%u] ssid=%s rssi=%d auth=%d channel=%u\n",
               i, (const char *)ap->ssid, ap->rssi, ap->authmode, ap->primary);
    }
    stop_wifi_session();
    return ESP_OK;
}

esp_err_t telegram_notifier_set_wifi_by_index(size_t index, const char *password)
{
    char ssid[sizeof(s_cfg.wifi_ssid)];

    if (!password || password[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (index >= s_scan_result_count || s_scan_results[index].ssid[0] == '\0') {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    copy_string(ssid, sizeof(ssid), (const char *)s_scan_results[index].ssid);
    xSemaphoreGive(s_lock);

    return telegram_notifier_set_wifi(ssid, password);
}

esp_err_t telegram_notifier_set_chat(const char *bot_token, const char *chat_id)
{
    esp_err_t err;

    if (!bot_token || !chat_id || bot_token[0] == '\0' || chat_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(bot_token) > TG_TOKEN_MAX_LEN || strlen(chat_id) > TG_CHAT_ID_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy_string(s_cfg.bot_token, sizeof(s_cfg.bot_token), bot_token);
    copy_string(s_cfg.chat_id, sizeof(s_cfg.chat_id), chat_id);
    err = save_config_locked();
    if (err == ESP_OK) {
        set_last_status_locked("chat config saved");
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t telegram_notifier_send_test(void)
{
    telegram_job_t job = {0};

    if (!s_job_queue || !s_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!config_complete_locked()) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_lock);

    snprintf(job.text, sizeof(job.text), "Telegram test t=%" PRIu64 "ms",
             esp_timer_get_time() / 1000ULL);
    return xQueueSend(s_job_queue, &job, pdMS_TO_TICKS(100)) == pdTRUE ?
               ESP_OK :
               ESP_ERR_TIMEOUT;
}

esp_err_t telegram_notifier_reset(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err;

    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    err = nvs_open(TG_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(nvs, TG_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg_loaded = false;
    s_wifi_connected = false;
    set_last_status_locked("config reset");
    xSemaphoreGive(s_lock);
    if (s_event_group) {
        xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
    }

    if (s_wifi_started) {
        esp_wifi_disconnect();
    }
    return ESP_OK;
}

void telegram_notifier_print_status(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    printf("telegram_configured=%s\n", config_complete_locked() ? "yes" : "no");
    printf("telegram_nvs_loaded=%s\n", s_cfg_loaded ? "yes" : "no");
    printf("telegram_wifi_ssid=%s\n", s_cfg.wifi_ssid[0] ? s_cfg.wifi_ssid : "<unset>");
    printf("telegram_bot_token=%s\n", s_cfg.bot_token[0] ? "<set>" : "<unset>");
    printf("telegram_chat_id=%s\n", s_cfg.chat_id[0] ? s_cfg.chat_id : "<unset>");
    printf("telegram_wifi_connected=%s\n", s_wifi_connected ? "yes" : "no");
    printf("telegram_last_status=%s\n", s_last_send_status[0] ? s_last_send_status : "<none>");
    xSemaphoreGive(s_lock);
}
