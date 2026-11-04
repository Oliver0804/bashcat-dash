#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct temperature_sensor_obj_t *temperature_sensor_handle_t;

typedef int temperature_sensor_clk_src_t;
#define TEMPERATURE_SENSOR_CLK_SRC_DEFAULT 0

typedef struct {
    int range_min;
    int range_max;
    temperature_sensor_clk_src_t clk_src;
    struct {
        uint32_t allow_pd;
    } flags;
} temperature_sensor_config_t;

#define TEMPERATURE_SENSOR_CONFIG_DEFAULT(min, max)    \
    {                                                  \
        .range_min = min,                              \
        .range_max = max,                              \
        .clk_src = TEMPERATURE_SENSOR_CLK_SRC_DEFAULT, \
        .flags = {                                     \
            .allow_pd = 0,                             \
        },                                             \
    }

esp_err_t temperature_sensor_install(const temperature_sensor_config_t *tsens_config,
                                     temperature_sensor_handle_t *ret_tsens);
esp_err_t temperature_sensor_enable(temperature_sensor_handle_t tsens);
esp_err_t temperature_sensor_get_celsius(temperature_sensor_handle_t tsens, float *out_celsius);
