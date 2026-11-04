#pragma once

#include <stdbool.h>

#include "bashcat_io.h"
#include "lvgl.h"

#define BASHCAT_PAGE_DASH   0
#define BASHCAT_PAGE_IO     1
#define BASHCAT_PAGE_I2C    2
#define BASHCAT_PAGE_CAN    3
#define BASHCAT_PAGE_RS485  4
#define BASHCAT_PAGE_SYS    5
#define BASHCAT_PAGE_ABOUT  6
#define BASHCAT_PAGE_COUNT  7

/* Implemented in bashcat_ui.c. True when a click on this page should act. */
bool bashcat_ui_page_ready(int page);

void bashcat_ui_v3_build(lv_obj_t *io, lv_obj_t *i2c, lv_obj_t *can, lv_obj_t *rs485);
void bashcat_ui_v3_enter(int page);
void bashcat_ui_v3_apply(const io_state_t *st);
