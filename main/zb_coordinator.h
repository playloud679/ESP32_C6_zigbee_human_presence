#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ZB_PROBE_ENDPOINT 1
#define ZB_PRIMARY_CHANNEL_MASK (1l << 15)

void zb_coordinator_run(void);
void zb_log_set_verbose(bool verbose);
bool zb_log_is_verbose(void);
