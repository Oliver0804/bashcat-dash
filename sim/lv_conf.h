/**
 * LVGL 8.4 config for the headless BASHCAT renderer.
 * Included via LV_CONF_INCLUDE_SIMPLE. Defaults for everything else
 * come from components/lvgl__lvgl/src/lv_conf_internal.h.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0
#define LV_COLOR_SCREEN_TRANSP 1

#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (2U * 1024U * 1024U)

#define LV_DISP_DEF_REFR_PERIOD 10
#define LV_INDEV_DEF_READ_PERIOD 10

#define LV_TICK_CUSTOM 0

#define LV_DRAW_COMPLEX 1
#define LV_SHADOW_CACHE_SIZE 0
#define LV_CIRCLE_CACHE_SIZE 8

#define LV_LAYER_SIMPLE_BUF_SIZE (256U * 1024U)
#define LV_LAYER_SIMPLE_FALLBACK_BUF_SIZE (32U * 1024U)

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_CHART 1
#define LV_USE_METER 1
#define LV_USE_QRCODE 1
#define LV_USE_SNAPSHOT 1

#define LV_USE_PERF_MONITOR 0

#endif
