#pragma once

#include <stdint.h>

int64_t esp_timer_get_time(void);

/* Host-only. The simulator advances this; the UI only reads it. */
void sim_timer_set_us(int64_t us);
