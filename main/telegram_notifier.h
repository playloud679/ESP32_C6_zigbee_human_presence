#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

void telegram_notifier_init(void);
void telegram_notifier_notify_presence(bool occupied);
esp_err_t telegram_notifier_set_wifi(const char *ssid, const char *password);
esp_err_t telegram_notifier_scan_wifi(void);
esp_err_t telegram_notifier_set_wifi_by_index(size_t index, const char *password);
esp_err_t telegram_notifier_set_chat(const char *bot_token, const char *chat_id);
esp_err_t telegram_notifier_send_test(void);
esp_err_t telegram_notifier_reset(void);
void telegram_notifier_print_status(void);
