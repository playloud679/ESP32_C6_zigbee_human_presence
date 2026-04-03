#pragma once

#include <stdint.h>

#define ZB_PROBE_ENDPOINT 1
#define ZB_PRIMARY_CHANNEL_MASK (1l << 15)

void zb_coordinator_run(void);
