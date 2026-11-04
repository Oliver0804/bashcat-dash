#pragma once

/*
 * Build the BASHCAT dash and play the boot intro.
 * Call once from app_main, while lvgl_port_lock() is held.
 * Later timers and animations run on the LVGL task.
 */
void bashcat_ui_start(void);
