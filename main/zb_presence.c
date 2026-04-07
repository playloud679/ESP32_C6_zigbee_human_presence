#include "zb_presence.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "esp_zigbee_core.h"
#include "zdo/esp_zigbee_zdo_command.h"

#include "presence_led.h"
#include "telegram_notifier.h"
#include "zb_coordinator.h"

static const char *TAG = "zb_probe_presence";
static const uint16_t ZB_VENDOR_QUERY_DELAY_MS = 1500;
static const uint16_t ZB_VENDOR_QUERY_RETRY_DELAY_MS = 12000;
static const uint8_t ZB_VENDOR_QUERY_CMD_ID = 0x03;
static const char *ZB_SENSOR_NVS_NS = "zb_probe";
static const char *ZB_SENSOR_NVS_KEY = "sensor_cfg";

typedef struct {
    uint16_t short_addr;
    uint8_t endpoint;
} simple_desc_ctx_t;

typedef struct {
    bool valid;
    uint8_t type;
    uint16_t len;
    uint32_t value;
} vendor_dp_entry_t;

typedef struct {
    bool captured;
    vendor_dp_entry_t entries[256];
} vendor_snapshot_t;

typedef struct {
    uint32_t magic;
    bool commissioned;
    bool has_vendor_cluster;
    uint16_t short_addr;
    uint8_t endpoint;
    uint16_t device_id;
    uint16_t cluster_id;
    uint8_t zone_id;
    uint16_t zone_type;
    zb_sensor_kind_t kind;
    esp_zb_ieee_addr_t ieee_addr;
} sensor_cfg_store_t;

static zb_presence_sensor_t s_sensor;
static vendor_snapshot_t s_clear_snapshot;
static vendor_snapshot_t s_present_snapshot;
static const uint32_t ZB_SENSOR_CFG_MAGIC = 0x5A424346UL;
static sensor_cfg_store_t s_persisted_cfg;
static bool s_persisted_cfg_valid;

static const char *sensor_kind_to_string(zb_sensor_kind_t kind)
{
    switch (kind) {
    case ZB_SENSOR_KIND_OCCUPANCY:
        return "occupancy";
    case ZB_SENSOR_KIND_IAS_ZONE:
        return "ias_zone";
    default:
        return "none";
    }
}

static const char *vendor_dp_name(uint8_t dp_id)
{
    switch (dp_id) {
    case 0x01:
        return "presence";
    case 0x02:
        return "sensitivity";
    case 0x04:
        return "hold_s";
    case 0x66:
        return "fading_s";
    case 0x6b:
        return "small_move";
    case 0x79:
        return "distance_cm";
    case 0x7a:
        return "large_move";
    case 0x7b:
        return "motion_state";
    case 0x7c:
        return "target_count";
    default:
        return NULL;
    }
}

static void log_vendor_dp_human(uint8_t dp_id, uint32_t value)
{
    const char *name = vendor_dp_name(dp_id);

    if (!name) {
        ESP_LOGI(TAG, "EF00 dp=0x%02x value=%" PRIu32, dp_id, value);
        return;
    }

    if (dp_id == 0x01U) {
        ESP_LOGI(TAG, "EF00 %-12s %s", name, value ? "PRESENT" : "CLEAR");
        return;
    }

    ESP_LOGI(TAG, "EF00 %-12s %" PRIu32, name, value);
}

static bool vendor_dp_changed(const vendor_dp_entry_t *entry, uint8_t dp_type,
                              uint16_t dp_len, uint32_t value)
{
    if (!entry->valid) {
        return true;
    }

    return entry->type != dp_type || entry->len != dp_len || entry->value != value;
}

static bool ieee_addr_is_zero(const esp_zb_ieee_addr_t ieee_addr)
{
    for (size_t i = 0; i < sizeof(esp_zb_ieee_addr_t); ++i) {
        if (ieee_addr[i] != 0U) {
            return false;
        }
    }
    return true;
}

static sensor_cfg_store_t sensor_cfg_from_runtime(void)
{
    sensor_cfg_store_t cfg = {
        .magic = ZB_SENSOR_CFG_MAGIC,
        .commissioned = s_sensor.commissioned,
        .has_vendor_cluster = s_sensor.has_vendor_cluster,
        .short_addr = s_sensor.short_addr,
        .endpoint = s_sensor.endpoint,
        .device_id = s_sensor.device_id,
        .cluster_id = s_sensor.cluster_id,
        .zone_id = s_sensor.zone_id,
        .zone_type = s_sensor.zone_type,
        .kind = s_sensor.kind,
    };

    memcpy(cfg.ieee_addr, s_sensor.ieee_addr, sizeof(cfg.ieee_addr));
    return cfg;
}

static void persist_sensor_cfg(void)
{
    sensor_cfg_store_t cfg = sensor_cfg_from_runtime();
    nvs_handle_t nvs = 0;
    esp_err_t err;

    if (s_persisted_cfg_valid &&
        memcmp(&s_persisted_cfg, &cfg, sizeof(cfg)) == 0) {
        return;
    }

    err = nvs_open(ZB_SENSOR_NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to open NVS for sensor config: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(nvs, ZB_SENSOR_NVS_KEY, &cfg, sizeof(cfg));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to persist sensor config: %s", esp_err_to_name(err));
    } else {
        s_persisted_cfg = cfg;
        s_persisted_cfg_valid = true;
    }
    nvs_close(nvs);
}

static void restore_sensor_cfg(void)
{
    sensor_cfg_store_t cfg = {0};
    nvs_handle_t nvs = 0;
    size_t size = sizeof(cfg);
    esp_err_t err = nvs_open(ZB_SENSOR_NVS_NS, NVS_READONLY, &nvs);

    if (err != ESP_OK) {
        return;
    }

    err = nvs_get_blob(nvs, ZB_SENSOR_NVS_KEY, &cfg, &size);
    nvs_close(nvs);
    if (err != ESP_OK || size != sizeof(cfg) || cfg.magic != ZB_SENSOR_CFG_MAGIC) {
        return;
    }

    s_sensor.commissioned = cfg.commissioned;
    s_sensor.has_vendor_cluster = cfg.has_vendor_cluster;
    s_sensor.short_addr = cfg.short_addr;
    s_sensor.endpoint = cfg.endpoint;
    s_sensor.device_id = cfg.device_id;
    s_sensor.cluster_id = cfg.cluster_id;
    s_sensor.zone_id = cfg.zone_id;
    s_sensor.zone_type = cfg.zone_type;
    s_sensor.kind = cfg.kind;
    memcpy(s_sensor.ieee_addr, cfg.ieee_addr, sizeof(s_sensor.ieee_addr));
    s_sensor.occupancy_known = false;
    s_sensor.occupied = false;
    s_sensor.zone_status = 0;

    s_persisted_cfg = cfg;
    s_persisted_cfg_valid = true;
    ESP_LOGI(TAG,
             "Restored sensor config short=0x%04hx ep=%u kind=%s cluster=0x%04hx ef00=%u",
             s_sensor.short_addr, s_sensor.endpoint, sensor_kind_to_string(s_sensor.kind),
             s_sensor.cluster_id, s_sensor.has_vendor_cluster);
}

static const char *vendor_dp_type_to_string(uint8_t type)
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

static bool vendor_dp_to_u32(uint8_t dp_type, uint16_t dp_len, const uint8_t *dp_value, uint32_t *out_value)
{
    uint32_t value = 0;

    if (!dp_value || !out_value || dp_len == 0U || dp_len > 4U) {
        return false;
    }
    if (!(dp_type == 0x01 || dp_type == 0x02 || dp_type == 0x04 || dp_type == 0x05)) {
        return false;
    }

    for (uint16_t i = 0; i < dp_len; ++i) {
        value = (value << 8) | dp_value[i];
    }
    *out_value = value;
    return true;
}

static vendor_snapshot_t *vendor_snapshot_for_state(bool present)
{
    return present ? &s_present_snapshot : &s_clear_snapshot;
}

static void print_vendor_dp_value(uint8_t dp_id, const vendor_dp_entry_t *entry, const char *label)
{
    if (!entry || !entry->valid) {
        printf("%s=<missing>", label);
        return;
    }

    printf("%s=dp=0x%02x type=%s len=%u value=%" PRIu32 " (0x%" PRIX32 ")",
           label, dp_id, vendor_dp_type_to_string(entry->type), entry->len,
           entry->value, entry->value);
}

static bool cluster_present_in_inputs(const esp_zb_af_simple_desc_1_1_t *simple_desc,
                                      uint16_t cluster_id)
{
    for (uint8_t i = 0; i < simple_desc->app_input_cluster_count; ++i) {
        if (simple_desc->app_cluster_list[i] == cluster_id) {
            return true;
        }
    }
    return false;
}

static void log_cluster_list(const esp_zb_af_simple_desc_1_1_t *simple_desc)
{
    if (!zb_log_is_verbose()) {
        return;
    }

    for (uint8_t i = 0; i < simple_desc->app_input_cluster_count; ++i) {
        ESP_LOGI(TAG, "  in_cluster[%u]=0x%04hx", i, simple_desc->app_cluster_list[i]);
    }
    for (uint8_t i = 0; i < simple_desc->app_output_cluster_count; ++i) {
        uint8_t idx = simple_desc->app_input_cluster_count + i;
        ESP_LOGI(TAG, "  out_cluster[%u]=0x%04hx", i, simple_desc->app_cluster_list[idx]);
    }
}

static bool update_presence_state(bool occupied)
{
    bool changed = !s_sensor.occupancy_known || s_sensor.occupied != occupied;

    s_sensor.occupancy_known = true;
    s_sensor.occupied = occupied;
    presence_led_set_state(occupied);
    if (changed) {
        ESP_LOGI(TAG, "Presence state -> %s", occupied ? "PRESENT" : "CLEAR");
        telegram_notifier_notify_presence(occupied);
    }
    return changed;
}

static void configure_occupancy_reporting(uint16_t short_addr, uint8_t endpoint)
{
    esp_zb_zcl_config_report_cmd_t report_cmd = {0};
    uint8_t occupancy_change = 1;

    esp_zb_zcl_config_report_record_t records[] = {
        {
            .direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
            .attributeID = ZB_OCCUPANCY_ATTR_ID,
            .attrType = ESP_ZB_ZCL_ATTR_TYPE_8BITMAP,
            .min_interval = 0,
            .max_interval = 30,
            .reportable_change = &occupancy_change,
        },
    };

    report_cmd.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    report_cmd.zcl_basic_cmd.src_endpoint = ZB_PROBE_ENDPOINT;
    report_cmd.zcl_basic_cmd.dst_endpoint = endpoint;
    report_cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    report_cmd.clusterID = ZB_OCCUPANCY_CLUSTER_ID;
    report_cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    report_cmd.record_number = 1;
    report_cmd.record_field = records;

    ESP_LOGI(TAG, "Configure reporting occupancy short=0x%04hx ep=%u",
             short_addr, endpoint);
    esp_zb_zcl_config_report_cmd_req(&report_cmd);
}

static void bind_cluster(uint16_t short_addr, uint8_t endpoint, uint16_t cluster_id,
                         const char *cluster_name)
{
    esp_zb_zdo_bind_req_param_t bind_req = {0};

    if (ieee_addr_is_zero(s_sensor.ieee_addr)) {
        esp_zb_ieee_address_by_short(short_addr, s_sensor.ieee_addr);
    }
    esp_zb_get_long_address(bind_req.dst_address_u.addr_long);
    memcpy(bind_req.src_address, s_sensor.ieee_addr, sizeof(esp_zb_ieee_addr_t));

    bind_req.src_endp = endpoint;
    bind_req.cluster_id = cluster_id;
    bind_req.dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED;
    bind_req.dst_endp = ZB_PROBE_ENDPOINT;
    bind_req.req_dst_addr = short_addr;

    if (zb_log_is_verbose()) {
        ESP_LOGI(TAG, "Bind %s cluster short=0x%04hx ep=%u", cluster_name, short_addr, endpoint);
    }
    esp_zb_zdo_device_bind_req(&bind_req, NULL, NULL);
}

static void send_vendor_query(uint8_t attempt)
{
    esp_zb_zcl_custom_cluster_cmd_req_t cmd_req = {0};
    uint8_t tsn;

    if (!s_sensor.commissioned || !s_sensor.has_vendor_cluster) {
        ESP_LOGI(TAG, "Skip EF00 query attempt=%u commissioned=%u ef00=%u",
                 attempt, s_sensor.commissioned, s_sensor.has_vendor_cluster);
        return;
    }

    cmd_req.zcl_basic_cmd.dst_addr_u.addr_short = s_sensor.short_addr;
    cmd_req.zcl_basic_cmd.src_endpoint = ZB_PROBE_ENDPOINT;
    cmd_req.zcl_basic_cmd.dst_endpoint = s_sensor.endpoint;
    cmd_req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd_req.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    cmd_req.cluster_id = ZB_VENDOR_CLUSTER_ID;
    cmd_req.manuf_specific = 0;
    cmd_req.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    cmd_req.dis_default_resp = 1;
    cmd_req.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
    cmd_req.custom_cmd_id = ZB_VENDOR_QUERY_CMD_ID;
    cmd_req.data.type = ESP_ZB_ZCL_ATTR_TYPE_NULL;
    cmd_req.data.size = 0;
    cmd_req.data.value = NULL;

    tsn = esp_zb_zcl_custom_cluster_cmd_req(&cmd_req);
    if (zb_log_is_verbose()) {
        ESP_LOGI(TAG,
                 "Sent EF00 query attempt=%u short=0x%04hx ep=%u cmd=0x%02x tsn=0x%02x",
                 attempt, s_sensor.short_addr, s_sensor.endpoint, ZB_VENDOR_QUERY_CMD_ID, tsn);
    }
}

static void vendor_query_cb(uint8_t param)
{
    send_vendor_query(param);
}

static void simple_desc_cb(esp_zb_zdp_status_t zdo_status,
                           esp_zb_af_simple_desc_1_1_t *simple_desc,
                           void *user_ctx)
{
    simple_desc_ctx_t *ctx = (simple_desc_ctx_t *)user_ctx;
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || !simple_desc || !ctx) {
        ESP_LOGW(TAG, "Simple desc failed for short=0x%04hx", ctx ? ctx->short_addr : 0xffff);
        free(ctx);
        return;
    }

    if (zb_log_is_verbose()) {
        ESP_LOGI(TAG,
                 "Endpoint %u profile=0x%04hx device=0x%04hx in=%u out=%u",
                 simple_desc->endpoint, simple_desc->app_profile_id,
                 simple_desc->app_device_id, simple_desc->app_input_cluster_count,
                 simple_desc->app_output_cluster_count);
    }
    log_cluster_list(simple_desc);

    if (!cluster_present_in_inputs(simple_desc, ZB_OCCUPANCY_CLUSTER_ID) &&
        !cluster_present_in_inputs(simple_desc, ZB_IAS_ZONE_CLUSTER_ID)) {
        free(ctx);
        return;
    }

    s_sensor.short_addr = ctx->short_addr;
    s_sensor.endpoint = ctx->endpoint;
    s_sensor.device_id = simple_desc->app_device_id;
    s_sensor.has_vendor_cluster = cluster_present_in_inputs(simple_desc, ZB_VENDOR_CLUSTER_ID);
    s_sensor.commissioned = true;

    if (cluster_present_in_inputs(simple_desc, ZB_OCCUPANCY_CLUSTER_ID)) {
        s_sensor.kind = ZB_SENSOR_KIND_OCCUPANCY;
        s_sensor.cluster_id = ZB_OCCUPANCY_CLUSTER_ID;
        ESP_LOGI(TAG, "Sensor discovered short=0x%04hx ep=%u kind=occupancy ef00=%s",
                 s_sensor.short_addr, s_sensor.endpoint,
                 s_sensor.has_vendor_cluster ? "yes" : "no");
        bind_cluster(s_sensor.short_addr, s_sensor.endpoint, ZB_OCCUPANCY_CLUSTER_ID, "occupancy");
        configure_occupancy_reporting(s_sensor.short_addr, s_sensor.endpoint);
    } else if (cluster_present_in_inputs(simple_desc, ZB_IAS_ZONE_CLUSTER_ID)) {
        s_sensor.kind = ZB_SENSOR_KIND_IAS_ZONE;
        s_sensor.cluster_id = ZB_IAS_ZONE_CLUSTER_ID;
        ESP_LOGI(TAG, "Sensor discovered short=0x%04hx ep=%u kind=ias_zone ef00=%s",
                 s_sensor.short_addr, s_sensor.endpoint,
                 s_sensor.has_vendor_cluster ? "yes" : "no");
        bind_cluster(s_sensor.short_addr, s_sensor.endpoint, ZB_IAS_ZONE_CLUSTER_ID, "ias_zone");
    }

    if (s_sensor.has_vendor_cluster) {
        if (zb_log_is_verbose()) {
            ESP_LOGI(TAG, "EF00 vendor cluster detected on short=0x%04hx ep=%u; scheduling queries",
                     s_sensor.short_addr, s_sensor.endpoint);
        }
        esp_zb_scheduler_alarm(vendor_query_cb, 1, ZB_VENDOR_QUERY_DELAY_MS);
        esp_zb_scheduler_alarm(vendor_query_cb, 2, ZB_VENDOR_QUERY_RETRY_DELAY_MS);
    }

    persist_sensor_cfg();

    free(ctx);
}

static void active_ep_cb(esp_zb_zdp_status_t zdo_status, uint8_t ep_count,
                         uint8_t *ep_id_list, void *user_ctx)
{
    uint16_t short_addr = (uint16_t)(uintptr_t)user_ctx;
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || !ep_id_list) {
        ESP_LOGW(TAG, "Active endpoint request failed for short=0x%04hx", short_addr);
        return;
    }

    if (zb_log_is_verbose()) {
        ESP_LOGI(TAG, "Device 0x%04hx exposes %u endpoint(s)", short_addr, ep_count);
    }
    for (uint8_t i = 0; i < ep_count; ++i) {
        esp_zb_zdo_simple_desc_req_param_t req = {
            .addr_of_interest = short_addr,
            .endpoint = ep_id_list[i],
        };
        simple_desc_ctx_t *ctx = calloc(1, sizeof(simple_desc_ctx_t));
        if (!ctx) {
            ESP_LOGE(TAG, "Out of memory requesting simple descriptor");
            return;
        }
        ctx->short_addr = short_addr;
        ctx->endpoint = ep_id_list[i];
        esp_zb_zdo_simple_desc_req(&req, simple_desc_cb, ctx);
    }
}

void zb_presence_init(void)
{
    memset(&s_sensor, 0, sizeof(s_sensor));
    memset(&s_clear_snapshot, 0, sizeof(s_clear_snapshot));
    memset(&s_present_snapshot, 0, sizeof(s_present_snapshot));
    presence_led_init();
    presence_led_set_unknown();
    memset(&s_persisted_cfg, 0, sizeof(s_persisted_cfg));
    s_persisted_cfg_valid = false;
    restore_sensor_cfg();
}

void zb_presence_on_device_announce(uint16_t short_addr)
{
    zb_presence_discover(short_addr);
}

void zb_presence_note_vendor_cluster(uint16_t short_addr, uint8_t endpoint)
{
    s_sensor.short_addr = short_addr;
    s_sensor.endpoint = endpoint;
    s_sensor.has_vendor_cluster = true;
    s_sensor.commissioned = true;
    persist_sensor_cfg();
}

void zb_presence_note_vendor_dp(uint16_t short_addr, uint8_t endpoint, uint8_t dp_id,
                                uint8_t dp_type, uint16_t dp_len, const uint8_t *dp_value)
{
    uint32_t value = 0;
    bool present_state = s_sensor.occupancy_known ? s_sensor.occupied : true;
    vendor_snapshot_t *snapshot = NULL;

    zb_presence_note_vendor_cluster(short_addr, endpoint);

    if (!vendor_dp_to_u32(dp_type, dp_len, dp_value, &value)) {
        return;
    }

    if (dp_id == 0x01U) {
        present_state = value != 0U;
        update_presence_state(present_state);
    }

    snapshot = vendor_snapshot_for_state(present_state);
    bool changed = vendor_dp_changed(&snapshot->entries[dp_id], dp_type, dp_len, value);
    snapshot->captured = true;
    snapshot->entries[dp_id].valid = true;
    snapshot->entries[dp_id].type = dp_type;
    snapshot->entries[dp_id].len = dp_len;
    snapshot->entries[dp_id].value = value;

    if (changed || zb_log_is_verbose()) {
        log_vendor_dp_human(dp_id, value);
    }
}

esp_err_t zb_presence_print_vendor_diff(void)
{
    bool found = false;

    if (!s_clear_snapshot.captured || !s_present_snapshot.captured) {
        printf("zb_diff requires both CLEAR and PRESENT EF00 snapshots\n");
        return ESP_ERR_INVALID_STATE;
    }

    for (uint16_t dp_id = 0; dp_id < 256U; ++dp_id) {
        const vendor_dp_entry_t *clear_entry = &s_clear_snapshot.entries[dp_id];
        const vendor_dp_entry_t *present_entry = &s_present_snapshot.entries[dp_id];

        if (!clear_entry->valid && !present_entry->valid) {
            continue;
        }
        if (clear_entry->valid == present_entry->valid &&
            clear_entry->type == present_entry->type &&
            clear_entry->len == present_entry->len &&
            clear_entry->value == present_entry->value) {
            continue;
        }

        print_vendor_dp_value((uint8_t)dp_id, clear_entry, "clear");
        printf("  ");
        print_vendor_dp_value((uint8_t)dp_id, present_entry, "present");
        printf("\n");
        found = true;
    }

    if (!found) {
        printf("zb_diff no EF00 differences between CLEAR and PRESENT\n");
    }

    return ESP_OK;
}

esp_err_t zb_presence_handle_report(const esp_zb_zcl_report_attr_message_t *message)
{
    uint8_t raw_value;
    bool occupied;

    if (!message || message->status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        return ESP_OK;
    }
    if (message->cluster != ZB_OCCUPANCY_CLUSTER_ID) {
        return ESP_OK;
    }
    if (message->attribute.id != ZB_OCCUPANCY_ATTR_ID || !message->attribute.data.value) {
        return ESP_OK;
    }

    raw_value = *(const uint8_t *)message->attribute.data.value;
    occupied = (raw_value & 0x01U) != 0;

    s_sensor.short_addr = message->src_address.u.short_addr;
    s_sensor.endpoint = message->src_endpoint;
    s_sensor.cluster_id = ZB_OCCUPANCY_CLUSTER_ID;
    s_sensor.kind = ZB_SENSOR_KIND_OCCUPANCY;
    update_presence_state(occupied);
    persist_sensor_cfg();

    ESP_LOGI(TAG,
             "OCCUPANCY short=0x%04hx ep=%u raw=0x%02x state=%s",
             s_sensor.short_addr, s_sensor.endpoint, raw_value,
             occupied ? "PRESENT" : "CLEAR");
    return ESP_OK;
}

const zb_presence_sensor_t *zb_presence_get_sensor(void)
{
    return &s_sensor;
}

esp_err_t zb_presence_discover(uint16_t short_addr)
{
    esp_zb_zdo_active_ep_req_param_t req = {
        .addr_of_interest = short_addr,
    };

    ESP_LOGI(TAG, "Query active endpoints for 0x%04hx", short_addr);
    esp_zb_zdo_active_ep_req(&req, active_ep_cb, (void *)(uintptr_t)short_addr);
    return ESP_OK;
}

esp_err_t zb_presence_bind_current(void)
{
    if (!s_sensor.commissioned) {
        return ESP_ERR_INVALID_STATE;
    }

    bind_cluster(s_sensor.short_addr, s_sensor.endpoint, s_sensor.cluster_id,
                 sensor_kind_to_string(s_sensor.kind));
    persist_sensor_cfg();
    return ESP_OK;
}

esp_err_t zb_presence_configure_reporting_current(void)
{
    if (!s_sensor.commissioned) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_sensor.kind != ZB_SENSOR_KIND_OCCUPANCY) {
        ESP_LOGI(TAG, "Reporting config is not used for sensor kind=%s",
                 sensor_kind_to_string(s_sensor.kind));
        return ESP_OK;
    }

    configure_occupancy_reporting(s_sensor.short_addr, s_sensor.endpoint);
    return ESP_OK;
}

esp_err_t zb_presence_query_vendor_current(void)
{
    if (!s_sensor.commissioned) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_sensor.has_vendor_cluster) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    send_vendor_query(0);
    return ESP_OK;
}

esp_err_t zb_presence_handle_ias_enroll_request(const esp_zb_zcl_ias_zone_enroll_request_message_t *message)
{
    esp_zb_zcl_ias_zone_enroll_response_cmd_t response = {0};

    if (!message || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        return ESP_OK;
    }

    s_sensor.short_addr = message->info.src_address.u.short_addr;
    s_sensor.endpoint = message->info.src_endpoint;
    s_sensor.cluster_id = ZB_IAS_ZONE_CLUSTER_ID;
    s_sensor.kind = ZB_SENSOR_KIND_IAS_ZONE;
    s_sensor.commissioned = true;
    s_sensor.zone_type = message->zone_type;
    s_sensor.zone_id = 0;
    persist_sensor_cfg();

    ESP_LOGI(TAG,
             "IAS enroll request short=0x%04hx ep=%u zone_type=0x%04hx manuf=0x%04hx",
             s_sensor.short_addr, s_sensor.endpoint, message->zone_type,
             message->manufacturer_code);

    response.zcl_basic_cmd.dst_addr_u.addr_short = s_sensor.short_addr;
    response.zcl_basic_cmd.src_endpoint = ZB_PROBE_ENDPOINT;
    response.zcl_basic_cmd.dst_endpoint = s_sensor.endpoint;
    response.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    response.enroll_rsp_code = ESP_ZB_ZCL_IAS_ZONE_ENROLL_RESPONSE_CODE_SUCCESS;
    response.zone_id = s_sensor.zone_id;
    esp_zb_zcl_ias_zone_enroll_cmd_resp(&response);

    ESP_LOGI(TAG, "IAS enroll response sent short=0x%04hx zone_id=%u",
             s_sensor.short_addr, s_sensor.zone_id);
    return ESP_OK;
}

esp_err_t zb_presence_handle_ias_status_change(const esp_zb_zcl_ias_zone_status_change_notification_message_t *message)
{
    bool occupied;

    if (!message || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        return ESP_OK;
    }

    occupied = (message->zone_status & ESP_ZB_ZCL_IAS_ZONE_ZONE_STATUS_ALARM1) != 0;

    s_sensor.short_addr = message->info.src_address.u.short_addr;
    s_sensor.endpoint = message->info.src_endpoint;
    s_sensor.cluster_id = ZB_IAS_ZONE_CLUSTER_ID;
    s_sensor.kind = ZB_SENSOR_KIND_IAS_ZONE;
    s_sensor.commissioned = true;
    update_presence_state(occupied);
    s_sensor.zone_status = message->zone_status;
    s_sensor.zone_id = message->zone_id;
    persist_sensor_cfg();

    ESP_LOGI(TAG,
             "IAS STATUS short=0x%04hx ep=%u zone_status=0x%04hx zone_id=%u state=%s",
             s_sensor.short_addr, s_sensor.endpoint, s_sensor.zone_status,
             s_sensor.zone_id, occupied ? "PRESENT" : "CLEAR");
    return ESP_OK;
}
