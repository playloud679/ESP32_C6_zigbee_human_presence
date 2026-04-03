#include "presence_led.h"

#include "esp_check.h"
#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "presence_led";

/*
 * Waveshare ESP32-C6-Zero exposes an onboard RGB LED and the board examples
 * use the addressable LED path on the ESP32-C6 family. GPIO8 matches the
 * default ESP-IDF C6 blink examples and works with the onboard RGB LED.
 */
static const int PRESENCE_LED_GPIO = 8;

static led_strip_handle_t s_led_strip;
static bool s_initialized;

static void presence_led_apply(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_initialized || !s_led_strip) {
        return;
    }

    ESP_ERROR_CHECK(led_strip_set_pixel(s_led_strip, 0, red, green, blue));
    ESP_ERROR_CHECK(led_strip_refresh(s_led_strip));
}

void presence_led_init(void)
{
    if (s_initialized) {
        return;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = PRESENCE_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip));
    ESP_ERROR_CHECK(led_strip_clear(s_led_strip));
    s_initialized = true;
    ESP_LOGI(TAG, "Onboard RGB LED ready on GPIO%d", PRESENCE_LED_GPIO);
}

void presence_led_set_unknown(void)
{
    if (!s_initialized || !s_led_strip) {
        return;
    }
    ESP_ERROR_CHECK(led_strip_clear(s_led_strip));
}

void presence_led_set_state(bool occupied)
{
    if (occupied) {
        /* Purple = red + blue */
        presence_led_apply(64, 0, 64);
    } else {
        /* White = all channels on */
        presence_led_apply(48, 48, 48);
    }
}
