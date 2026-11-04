/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "waveshare_rgb_lcd_port.h"
#include "bashcat_io.h"
#include "bashcat_ui.h"

void app_main(void)
{
    waveshare_esp32_s3_rgb_lcd_init();
    bashcat_io_start();

    ESP_LOGI(TAG, "Start BASHCAT dash");
    if (lvgl_port_lock(-1)) {
        bashcat_ui_start();
        lvgl_port_unlock();
    }
}
