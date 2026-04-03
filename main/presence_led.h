#pragma once

#include <stdbool.h>

void presence_led_init(void);
void presence_led_set_unknown(void);
void presence_led_set_state(bool occupied);
