#include "zb_coordinator.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"

#include "zboss_api.h"
#include "esp_zigbee_attribute.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_endpoint.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "nwk/esp_zigbee_nwk.h"
#include "zdo/esp_zigbee_zdo_command.h"
#include "zcl/zb_zcl_common.h"

#include "zb_presence.h"

static const char *TAG = "zb_probe_coord";

static const char *tuya_dp_type_to_string(uint8_t type)
{
    switch (type) {
    case 0x00:
        return "raw";
    case 0x01:
        return "bool";
    case 0x02:
        return "value";
    case 0x03:
        return "string";
    case 0x04:
        return "enum";
    case 0x05:
        return "bitmap";
    default:
        return "unknown";
    }
}

static void bytes_to_hex_string(const uint8_t *data, uint16_t data_len, char *out, size_t out_len)
{
    if (!out_len) {
        return;
    }

    if (!data || !data_len) {
        out[0] = '\0';
        return;
    }

    size_t pos = 0;
    for (uint16_t i = 0; i < data_len && (pos + 4) < out_len; ++i) {
        pos += (size_t)snprintf(out + pos, out_len - pos, "%02X ", data[i]);
    }

    out[pos ? pos - 1 : 0] = '\0';
}

static void log_tuya_dp_records(uint16_t short_addr, uint8_t src_endpoint,
                                const uint8_t *payload, uint16_t payload_len)
{
    uint16_t offset = 0;

    while ((offset + 6U) <= payload_len) {
        uint16_t seq = ((uint16_t)payload[offset] << 8) | payload[offset + 1U];
        uint8_t dp_id = payload[offset + 2U];
        uint8_t dp_type = payload[offset + 3U];
        uint16_t dp_len = ((uint16_t)payload[offset + 4U] << 8) | payload[offset + 5U];
        const uint8_t *dp_value = payload + offset + 6U;

        if ((offset + 6U + dp_len) > payload_len) {
            ESP_LOGW(TAG,
                     "RAW EF00 parse error seq=0x%04x dp=0x%02x type=0x%02x len=%u exceeds payload_len=%u",
                     seq, dp_id, dp_type, dp_len, payload_len);
            return;
        }

        zb_presence_note_vendor_dp(short_addr, src_endpoint, dp_id, dp_type, dp_len, dp_value);

        if (dp_type == 0x01 && dp_len == 1U) {
            ESP_LOGI(TAG,
                     "RAW EF00 dp seq=0x%04x dp=0x%02x type=%s len=%u value=%u",
                     seq, dp_id, tuya_dp_type_to_string(dp_type), dp_len, dp_value[0]);
        } else if ((dp_type == 0x02 || dp_type == 0x05) && dp_len <= 4U && dp_len > 0U) {
            uint32_t value = 0;
            for (uint16_t i = 0; i < dp_len; ++i) {
                value = (value << 8) | dp_value[i];
            }
            ESP_LOGI(TAG,
                     "RAW EF00 dp seq=0x%04x dp=0x%02x type=%s len=%u value=%" PRIu32 " (0x%" PRIX32 ")",
                     seq, dp_id, tuya_dp_type_to_string(dp_type), dp_len, value, value);
        } else if (dp_type == 0x04 && dp_len == 1U) {
            ESP_LOGI(TAG,
                     "RAW EF00 dp seq=0x%04x dp=0x%02x type=%s len=%u value=%u",
                     seq, dp_id, tuya_dp_type_to_string(dp_type), dp_len, dp_value[0]);
        } else {
            char value_hex[3 * 32 + 1];
            uint16_t used = dp_len > 32U ? 32U : dp_len;
            bytes_to_hex_string(dp_value, used, value_hex, sizeof(value_hex));
            ESP_LOGI(TAG,
                     "RAW EF00 dp seq=0x%04x dp=0x%02x type=%s len=%u value_hex=%s%s",
                     seq, dp_id, tuya_dp_type_to_string(dp_type), dp_len,
                     used ? value_hex : "<empty>", dp_len > used ? " ..." : "");
        }

        offset = (uint16_t)(offset + 6U + dp_len);
    }

    if (offset != payload_len) {
        char tail_hex[3 * 16 + 1];
        uint16_t tail_len = payload_len - offset;
        uint16_t used = tail_len > 16U ? 16U : tail_len;
        bytes_to_hex_string(payload + offset, used, tail_hex, sizeof(tail_hex));
        ESP_LOGW(TAG, "RAW EF00 trailing bytes[%u]=%s%s",
                 tail_len, used ? tail_hex : "<empty>", tail_len > used ? " ..." : "");
    }
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    if (esp_zb_bdb_start_top_level_commissioning(mode_mask) != ESP_OK) {
        ESP_LOGW(TAG, "Unable to restart commissioning, mode=0x%02x", mode_mask);
    }
}

static bool zb_raw_command_handler(uint8_t bufid)
{
    zb_zcl_parsed_hdr_t *cmd_info = ZB_BUF_GET_PARAM(bufid, zb_zcl_parsed_hdr_t);
    uint8_t *payload = zb_buf_begin(bufid);
    uint16_t payload_len = zb_buf_len(bufid);

    if (!cmd_info || cmd_info->cluster_id != 0xEF00) {
        return false;
    }

    ESP_LOGI(TAG,
             "RAW EF00 short=0x%04hx src_ep=%u dst_ep=%u profile=0x%04hx cmd=0x%02x len=%u manuf=%u",
             cmd_info->addr_data.common_data.source.u.short_addr,
             cmd_info->addr_data.common_data.src_endpoint,
             cmd_info->addr_data.common_data.dst_endpoint,
             cmd_info->profile_id,
             cmd_info->cmd_id,
             payload_len,
             cmd_info->is_manuf_specific ? cmd_info->manuf_specific : 0);

    zb_presence_note_vendor_cluster(cmd_info->addr_data.common_data.source.u.short_addr,
                                    cmd_info->addr_data.common_data.src_endpoint);

    if (payload && payload_len) {
        char hex[3 * 64 + 1];
        uint16_t used = payload_len > 64 ? 64 : payload_len;
        bytes_to_hex_string(payload, used, hex, sizeof(hex));
        ESP_LOGI(TAG, "RAW EF00 payload[%u]=%s%s",
                 used, hex, payload_len > used ? " ..." : "");
        log_tuya_dp_records(cmd_info->addr_data.common_data.source.u.short_addr,
                            cmd_info->addr_data.common_data.src_endpoint,
                            payload, payload_len);
    }

    return false;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    switch (callback_id) {
    case ESP_ZB_CORE_REPORT_ATTR_CB_ID:
        return zb_presence_handle_report((const esp_zb_zcl_report_attr_message_t *)message);

    case ESP_ZB_CORE_CMD_IAS_ZONE_ZONE_ENROLL_REQUEST_ID:
        return zb_presence_handle_ias_enroll_request(
            (const esp_zb_zcl_ias_zone_enroll_request_message_t *)message);

    case ESP_ZB_CORE_CMD_IAS_ZONE_ZONE_STATUS_CHANGE_NOT_ID:
        return zb_presence_handle_ias_status_change(
            (const esp_zb_zcl_ias_zone_status_change_notification_message_t *)message);

    case ESP_ZB_CORE_CMD_REPORT_CONFIG_RESP_CB_ID:
        ESP_LOGI(TAG, "Configure reporting response received");
        return ESP_OK;

    case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID:
        ESP_LOGI(TAG, "Default ZCL response received");
        return ESP_OK;

    default:
        ESP_LOGD(TAG, "Unhandled action callback id=%d", callback_id);
        return ESP_OK;
    }
}

static void zb_register_probe_endpoint(void)
{
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();

    esp_zb_attribute_list_t *basic_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_BASIC);
    esp_zb_attribute_list_t *identify_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_IDENTIFY);
    esp_zb_attribute_list_t *occupancy_cluster =
        esp_zb_zcl_attr_list_create(ZB_OCCUPANCY_CLUSTER_ID);
    esp_zb_attribute_list_t *ias_zone_cluster =
        esp_zb_zcl_attr_list_create(ZB_IAS_ZONE_CLUSTER_ID);
    esp_zb_attribute_list_t *vendor_cluster =
        esp_zb_zcl_attr_list_create(ZB_VENDOR_CLUSTER_ID);

    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = ZB_PROBE_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = 0x0000,
        .app_device_version = 0,
    };

    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(
        cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(
        cluster_list, identify_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_occupancy_sensing_cluster(
        cluster_list, occupancy_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_ias_zone_cluster(
        cluster_list, ias_zone_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_custom_cluster(
        cluster_list, vendor_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE));
    ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(ep_list, cluster_list, endpoint_config));
    ESP_ERROR_CHECK(esp_zb_device_register(ep_list));
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *signal = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t signal_type = *signal;

    switch (signal_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Coordinator booted in%s factory-new mode",
                     esp_zb_bdb_is_factory_new() ? "" : " non");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Starting network formation");
                esp_zb_bdb_start_top_level_commissioning(
                    ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                ESP_LOGI(TAG, "Reopened existing network for joining");
                esp_zb_bdb_open_network(180);
            }
        } else {
            ESP_LOGW(TAG, "Startup failed: %s", esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_INITIALIZATION, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t ext_pan_id;
            esp_zb_get_extended_pan_id(ext_pan_id);
            ESP_LOGI(TAG,
                     "Network formed PAN=0x%04hx channel=%d short=0x%04hx",
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(),
                     esp_zb_get_short_address());
            ESP_LOGI(TAG,
                     "Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                     ext_pan_id[7], ext_pan_id[6], ext_pan_id[5], ext_pan_id[4],
                     ext_pan_id[3], ext_pan_id[2], ext_pan_id[1], ext_pan_id[0]);
            esp_zb_bdb_start_top_level_commissioning(
                ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGW(TAG, "Formation failed: %s", esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Permit join active");
        } else {
            ESP_LOGW(TAG, "Steering failed: %s", esp_err_to_name(err_status));
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        esp_zb_zdo_signal_device_annce_params_t *params =
            (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(signal);
        ESP_LOGI(TAG, "Device announce short=0x%04hx", params->device_short_addr);
        zb_presence_on_device_announce(params->device_short_addr);
        break;
    }

    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        uint8_t seconds = *(uint8_t *)esp_zb_app_signal_get_params(signal);
        ESP_LOGI(TAG, "Permit join %s (%us remaining)",
                 seconds ? "open" : "closed", seconds);
        break;
    }

    default:
        ESP_LOGI(TAG, "ZDO signal %s (0x%x) status=%s",
                 esp_zb_zdo_signal_to_string(signal_type),
                 signal_type, esp_err_to_name(err_status));
        break;
    }
}

void zb_coordinator_run(void)
{
    esp_zb_cfg_t zigbee_config = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg = {
            .max_children = 10,
        },
    };

    zb_presence_init();
    esp_zb_init(&zigbee_config);
    zb_register_probe_endpoint();
    esp_zb_raw_command_handler_register(zb_raw_command_handler);
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(ZB_PRIMARY_CHANNEL_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}
