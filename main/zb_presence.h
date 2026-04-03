#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_zigbee_type.h"
#include "zcl/esp_zigbee_zcl_ias_zone.h"
#include "zcl/esp_zigbee_zcl_core.h"
#include "zcl/esp_zigbee_zcl_command.h"

#define ZB_OCCUPANCY_CLUSTER_ID 0x0406
#define ZB_OCCUPANCY_ATTR_ID 0x0000
#define ZB_IAS_ZONE_CLUSTER_ID 0x0500
#define ZB_VENDOR_CLUSTER_ID 0xEF00

typedef enum {
    ZB_SENSOR_KIND_NONE = 0,
    ZB_SENSOR_KIND_OCCUPANCY = 1,
    ZB_SENSOR_KIND_IAS_ZONE = 2,
} zb_sensor_kind_t;

typedef struct {
    bool commissioned;
    bool occupancy_known;
    bool occupied;
    bool has_vendor_cluster;
    uint16_t short_addr;
    uint8_t endpoint;
    uint16_t device_id;
    uint16_t cluster_id;
    uint8_t zone_id;
    uint16_t zone_type;
    uint16_t zone_status;
    zb_sensor_kind_t kind;
    esp_zb_ieee_addr_t ieee_addr;
} zb_presence_sensor_t;

void zb_presence_init(void);
void zb_presence_on_device_announce(uint16_t short_addr);
esp_err_t zb_presence_handle_report(const esp_zb_zcl_report_attr_message_t *message);
esp_err_t zb_presence_handle_ias_enroll_request(const esp_zb_zcl_ias_zone_enroll_request_message_t *message);
esp_err_t zb_presence_handle_ias_status_change(const esp_zb_zcl_ias_zone_status_change_notification_message_t *message);
const zb_presence_sensor_t *zb_presence_get_sensor(void);
esp_err_t zb_presence_discover(uint16_t short_addr);
esp_err_t zb_presence_bind_current(void);
esp_err_t zb_presence_configure_reporting_current(void);
esp_err_t zb_presence_query_vendor_current(void);
void zb_presence_note_vendor_cluster(uint16_t short_addr, uint8_t endpoint);
void zb_presence_note_vendor_dp(uint16_t short_addr, uint8_t endpoint, uint8_t dp_id,
                                uint8_t dp_type, uint16_t dp_len, const uint8_t *dp_value);
esp_err_t zb_presence_print_vendor_diff(void);
