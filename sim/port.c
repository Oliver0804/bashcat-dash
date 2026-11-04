/*
 * ESP-IDF / FreeRTOS stand-ins for the desktop renderer.
 * The UI is compiled unchanged and only sees the public headers.
 */

#include "driver/temperature_sensor.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* SRAM 400 KiB total, 180 KiB free → 45%. PSRAM 8 MiB total, 5.1 MiB free. */
#define SIM_SRAM_TOTAL (400u * 1024u)
#define SIM_SRAM_FREE (180u * 1024u)
#define SIM_PSRAM_TOTAL (8u * 1024u * 1024u)
/* Smallest integer whose (free * 10) / 1 MiB prints as 51, i.e. 5.1 MB. */
#define SIM_PSRAM_FREE ((51u * 1024u * 1024u + 9u) / 10u)

_Static_assert(SIM_SRAM_FREE * 100ull / SIM_SRAM_TOTAL == 45ull, "sram free percent");
_Static_assert((SIM_PSRAM_FREE * 10ull) / (1024ull * 1024ull) == 51ull, "psram free x10");

static int64_t s_now_us;

void sim_timer_set_us(int64_t us)
{
    s_now_us = us;
}

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

const char *esp_err_to_name(esp_err_t code)
{
    if (code == ESP_OK) {
        return "ESP_OK";
    }
    return "ESP_FAIL";
}

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

void heap_caps_free(void *ptr)
{
    free(ptr);
}

size_t heap_caps_get_free_size(uint32_t caps)
{
    if (caps & MALLOC_CAP_INTERNAL) {
        return SIM_SRAM_FREE;
    }
    if (caps & MALLOC_CAP_SPIRAM) {
        return SIM_PSRAM_FREE;
    }
    return 0;
}

size_t heap_caps_get_total_size(uint32_t caps)
{
    if (caps & MALLOC_CAP_INTERNAL) {
        return SIM_SRAM_TOTAL;
    }
    if (caps & MALLOC_CAP_SPIRAM) {
        return SIM_PSRAM_TOTAL;
    }
    return 0;
}

static int s_tsens_alive;

esp_err_t temperature_sensor_install(const temperature_sensor_config_t *tsens_config,
                                     temperature_sensor_handle_t *ret_tsens)
{
    (void)tsens_config;
    if (ret_tsens == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_tsens_alive = 1;
    *ret_tsens = (temperature_sensor_handle_t)(intptr_t)1;
    return ESP_OK;
}

esp_err_t temperature_sensor_enable(temperature_sensor_handle_t tsens)
{
    (void)tsens;
    if (!s_tsens_alive) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t temperature_sensor_get_celsius(temperature_sensor_handle_t tsens, float *out_celsius)
{
    (void)tsens;
    if (!s_tsens_alive || out_celsius == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_celsius = 42.5f;
    return ESP_OK;
}

/*
 * CPU load, percent, slowly across 30..90 so the arc crosses the 80% warn color
 * while the main gauge is still showing CPU (before the t=7000 tap):
 *   0..4600 ms     34..46
 *   4600..5200 ms  46 → 88
 *   5200..7000 ms  86..90
 *   7000..12000 ms 90 → 30
 * The idle counter integrates that curve, so each 500 ms sample is the average.
 */
static int sim_cpu_load_pct(int64_t t_ms)
{
    if (t_ms < 0) {
        t_ms = 0;
    }
    if (t_ms < 4600) {
        int phase = (int)(t_ms % 2000);
        int tri = (phase < 1000) ? phase : (2000 - phase);
        return 34 + tri * 12 / 1000;
    }
    if (t_ms < 5200) {
        return 46 + (int)((t_ms - 4600) * 42 / 600);
    }
    if (t_ms < 7000) {
        int phase = (int)((t_ms - 5200) % 1000);
        int tri = (phase < 500) ? phase : (1000 - phase);
        return 86 + tri * 4 / 500;
    }
    if (t_ms >= 12000) {
        return 30;
    }
    return 90 - (int)((t_ms - 7000) * 60 / 5000);
}

static uint32_t s_idle[2];
static int64_t s_idle_stamp[2];

configRUN_TIME_COUNTER_TYPE ulTaskGetIdleRunTimeCounterForCore(BaseType_t xCoreID)
{
    int idx = (xCoreID <= 0) ? 0 : 1;
    if (idx > 1) {
        idx = 1;
    }
    int64_t now = s_now_us;
    int64_t dt = now - s_idle_stamp[idx];
    if (dt > 0) {
        int load = (sim_cpu_load_pct(s_idle_stamp[idx] / 1000) + sim_cpu_load_pct(now / 1000)) / 2;
        if (load < 0) {
            load = 0;
        }
        if (load > 100) {
            load = 100;
        }
        s_idle[idx] += (uint32_t)(((uint64_t)dt * (uint64_t)(100 - load)) / 100ull);
        s_idle_stamp[idx] = now;
    }
    return s_idle[idx];
}

void esp_chip_info(esp_chip_info_t *out_info)
{
    if (out_info == NULL) {
        return;
    }
    out_info->model = CHIP_ESP32S3;
    out_info->features = 0;
    out_info->revision = 200; /* M.XX → 2.0 */
    out_info->cores = 2;
}

esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *out_size)
{
    (void)chip;
    if (out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_size = 16u * 1024u * 1024u;
    return ESP_OK;
}

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    (void)type;
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    mac[0] = 0x02;
    mac[1] = 0x00;
    mac[2] = 0x00;
    mac[3] = 0x00;
    mac[4] = 0x00;
    mac[5] = 0x01;
    return ESP_OK;
}

const char *esp_get_idf_version(void)
{
    return "v5.5.2";
}

esp_reset_reason_t esp_reset_reason(void)
{
    return ESP_RST_POWERON;
}

uint32_t esp_get_minimum_free_heap_size(void)
{
    return 160u * 1024u;
}
