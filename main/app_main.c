#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_zigbee_core.h"
#include "platform/esp_zigbee_platform.h"

#include "zb_cli.h"
#include "zb_coordinator.h"

static const char *TAG = "zb_probe_main";

static void zigbee_task(void *pvParameters)
{
    (void)pvParameters;
    zb_coordinator_run();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    esp_zb_platform_config_t config = {
        .radio_config = {
            .radio_mode = ZB_RADIO_MODE_NATIVE,
        },
        .host_config = {
            .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,
        },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    zb_cli_start();

    BaseType_t ok = xTaskCreate(zigbee_task, "zigbee_main", 8192, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, , TAG, "Unable to create Zigbee task");
}
