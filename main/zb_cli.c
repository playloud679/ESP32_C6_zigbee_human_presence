#include "zb_cli.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "platform/esp_zigbee_platform.h"

#include "zb_coordinator.h"
#include "zb_presence.h"

static const char *TAG = "zb_probe_cli";

static uint16_t parse_short_addr_or_default(const char *arg, uint16_t fallback)
{
    if (!arg) {
        return fallback;
    }
    return (uint16_t)strtoul(arg, NULL, 0);
}

static int cmd_zb_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const zb_presence_sensor_t *sensor = zb_presence_get_sensor();
    esp_zb_lock_acquire(portMAX_DELAY);
    printf("zigbee_started=%s\n", esp_zb_is_started() ? "yes" : "no");
    printf("network_joined=%s\n", esp_zb_bdb_dev_joined() ? "yes" : "no");
    if (esp_zb_is_started()) {
        printf("pan_id=0x%04x channel=%d short=0x%04x\n",
               esp_zb_get_pan_id(),
               esp_zb_get_current_channel(),
               esp_zb_get_short_address());
    }
    esp_zb_lock_release();

    printf("sensor_commissioned=%s short=0x%04x ep=%u\n",
           sensor->commissioned ? "yes" : "no",
           sensor->short_addr,
           sensor->endpoint);
    printf("sensor_kind=%d cluster=0x%04x device=0x%04x zone_type=0x%04x zone_status=0x%04x zone_id=%u\n",
           sensor->kind,
           sensor->cluster_id,
           sensor->device_id,
           sensor->zone_type,
           sensor->zone_status,
           sensor->zone_id);
    if (sensor->occupancy_known) {
        printf("occupancy=%s\n", sensor->occupied ? "PRESENT" : "CLEAR");
    } else {
        printf("occupancy=unknown\n");
    }
    return 0;
}

static int cmd_zb_open(int argc, char **argv)
{
    uint8_t seconds = 180;
    if (argc >= 2) {
        seconds = (uint8_t)strtoul(argv[1], NULL, 0);
    }

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t err = esp_zb_bdb_open_network(seconds);
    esp_zb_lock_release();

    printf("zb_open seconds=%u err=%s\n", seconds, esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_zb_close(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t err = esp_zb_bdb_close_network();
    esp_zb_lock_release();

    printf("zb_close err=%s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_zb_sensor(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const zb_presence_sensor_t *sensor = zb_presence_get_sensor();
    printf("sensor commissioned=%s short=0x%04x ep=%u kind=%d cluster=0x%04x device=0x%04x zone_type=0x%04x zone_status=0x%04x zone_id=%u occupancy_known=%s state=%s\n",
           sensor->commissioned ? "yes" : "no",
           sensor->short_addr,
           sensor->endpoint,
           sensor->kind,
           sensor->cluster_id,
           sensor->device_id,
           sensor->zone_type,
           sensor->zone_status,
           sensor->zone_id,
           sensor->occupancy_known ? "yes" : "no",
           sensor->occupancy_known ? (sensor->occupied ? "PRESENT" : "CLEAR") : "unknown");
    return 0;
}

static int cmd_zb_discover(int argc, char **argv)
{
    const zb_presence_sensor_t *sensor = zb_presence_get_sensor();
    uint16_t short_addr = parse_short_addr_or_default(argc >= 2 ? argv[1] : NULL,
                                                      sensor->short_addr);

    if (short_addr == 0) {
        printf("usage: zb_discover <0xSHORT>\n");
        return 1;
    }

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t err = zb_presence_discover(short_addr);
    esp_zb_lock_release();

    printf("zb_discover short=0x%04x err=%s\n", short_addr, esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_zb_bind(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t err = zb_presence_bind_current();
    esp_zb_lock_release();

    printf("zb_bind err=%s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_zb_report(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t err = zb_presence_configure_reporting_current();
    esp_zb_lock_release();

    printf("zb_report err=%s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_zb_query(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t err = zb_presence_query_vendor_current();
    esp_zb_lock_release();

    printf("zb_query err=%s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_zb_diff(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_err_t err = zb_presence_print_vendor_diff();
    return err == ESP_OK ? 0 : 1;
}

static int cmd_zb_factory_reset(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("factory reset requested, device will restart\n");
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_factory_reset();
    esp_zb_lock_release();
    return 0;
}

static void register_command(const char *command, const char *help,
                             const char *hint, esp_console_cmd_func_t func)
{
    const esp_console_cmd_t cmd = {
        .command = command,
        .help = help,
        .hint = hint,
        .func = func,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void zb_cli_start(void)
{
    static bool started = false;
    if (started) {
        return;
    }

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "zb> ";

    esp_console_register_help_command();
    register_command("zb_status", "Show network and sensor status", NULL, cmd_zb_status);
    register_command("zb_open", "Open permit join window", "[seconds]", cmd_zb_open);
    register_command("zb_close", "Close permit join window", NULL, cmd_zb_close);
    register_command("zb_sensor", "Show cached sensor state", NULL, cmd_zb_sensor);
    register_command("zb_discover", "Discover endpoints for short address", "[0xshort]", cmd_zb_discover);
    register_command("zb_bind", "Bind current occupancy sensor", NULL, cmd_zb_bind);
    register_command("zb_report", "Configure occupancy reporting", NULL, cmd_zb_report);
    register_command("zb_query", "Send EF00 vendor status query", NULL, cmd_zb_query);
    register_command("zb_diff", "Show EF00 differences between CLEAR and PRESENT", NULL, cmd_zb_diff);
    register_command("zb_factory_reset", "Erase Zigbee storage and restart", NULL, cmd_zb_factory_reset);

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#else
#error Unsupported console type
#endif

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "CLI ready. Type 'help' for commands.");
    started = true;
}
