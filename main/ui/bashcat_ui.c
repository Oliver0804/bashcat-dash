/*
 * BASHCAT dash — LVGL 8.4 instrument UI for the 800x480 panel.
 * Pages, overlay, and the idle layer are built once. Switching only
 * moves or hides them. All LVGL calls run under the port lock.
 */

#include "bashcat_ui.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/temperature_sensor.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "src/extra/others/snapshot/lv_snapshot.h"
#include "bashcat_io.h"
#include "bashcat_ui_v3.h"

#ifndef CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 240
#endif

#if !defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) || !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#error "sdkconfig needs CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS"
#endif
#if LV_FONT_MONTSERRAT_48 == 0
#error "sdkconfig needs CONFIG_LV_FONT_MONTSERRAT_48"
#endif
#if LV_USE_METER == 0 || LV_USE_CHART == 0
#error "LV_USE_METER and LV_USE_CHART are required"
#endif
#if LV_USE_QRCODE == 0
#error "sdkconfig needs CONFIG_LV_USE_QRCODE"
#endif
#if LV_USE_SNAPSHOT == 0
#error "LV_USE_SNAPSHOT is required"
#endif

static const char *TAG_UI = "bashcat";

/* ---- palette (bashcat.net) ---- */
#define HEX_BG     0x0B0D0F
#define HEX_PANEL  0x111417
#define HEX_PRESS  0x151A1E
#define HEX_LINE   0x1E2328
#define HEX_TEXT   0xE3E7EA
#define HEX_DIM    0x99A2AA
#define HEX_MUTED  0x6B747C
#define HEX_ACCENT 0x34C77B
#define HEX_WARN   0xF5A524
#define HEX_CRIT   0xE5484D

/* ---- timing, documented in NOTES.md ---- */
#define UI_SAMPLE_MS         500
#define UI_VALUE_ANIM_MS     400
#define UI_COLOR_ANIM_MS     380
#define UI_SWEEP_UP_MS       900
#define UI_SWEEP_DOWN_MS     800
#define UI_TILE_FADE_MS      280
#define UI_TILE_STAGGER_MS   80
#define UI_BREATH_HALF_MS    1000
#define UI_INTRO_IN_MS       720
#define UI_INTRO_GAP1_MS     80
#define UI_INTRO_LINE_MS     860
#define UI_INTRO_GAP2_MS     160
#define UI_INTRO_OUT_MS      680
#define UI_CPU_WARN          80
#define UI_CPU_CRIT          95
#define UI_CHART_POINTS      60
#define UI_LINE_W            420
#define UI_PAGE_MS           320
#define UI_TRACK_MS          300
#define UI_PAGE_STAGGER_MS   40
#define UI_PAGE_SLIDE_PX     16
#define UI_ENTER_MS          280
#define UI_RESWEEP_MS        500
#define UI_RETRACT_MS        200
#define UI_SWEEP_TO_MS       400
#define UI_PRESS_MS          120
#define UI_OVERLAY_MS        300
#define UI_IDLE_TIMEOUT_MS   60000
#define UI_IDLE_FADE_MS      400
#define UI_IDLE_LINE_HALF_MS 1600
#define UI_ROLL_MS           280
#define UI_ABOUT_ZOOM_START  230
#define UI_QR_SIZE           168
#define UI_QR_QUIET          12
#define UI_QR_CARD           (UI_QR_SIZE + (UI_QR_QUIET * 2))
#define UI_DOT_OFF_W         6
#define UI_DOT_ON_W          18
#define UI_DOT_H             6
#define UI_DOT_GAP           6
#define UI_HEADER_H          52
#define UI_MASK_OPA          178
#define UI_PAGE_COUNT        BASHCAT_PAGE_COUNT
#define UI_SYS_PITCH         30
#define UI_IO_POLL_MS        100

#define UI_W 800
#define UI_H 480

#define UI_CPU_CORES 2
#define BULLET "\xE2\x80\xA2"

typedef enum {
    PHASE_INTRO = 0,
    PHASE_SWEEP,
    PHASE_LIVE,
} phase_t;

typedef enum {
    METRIC_CPU = 0,
    METRIC_SRAM,
    METRIC_PSRAM,
    METRIC_TEMP,
    METRIC_COUNT,
} metric_t;

typedef enum {
    TILE_HEAP = 0,
    TILE_PSRAM,
    TILE_TEMP,
    TILE_FPS,
    TILE_COUNT,
} tile_kind_t;

typedef enum {
    SYS_CHIP = 0,
    SYS_CORES,
    SYS_CPU,
    SYS_FLASH,
    SYS_PSRAM,
    SYS_MAC,
    SYS_IDF,
    SYS_LVGL,
    SYS_RESET,
    SYS_UPTIME,
    SYS_RTC,
    SYS_PERIPH,
    SYS_MINHEAP,
    SYS_COUNT,
} sys_id_t;

typedef struct {
    lv_obj_t *clip;
    lv_obj_t *lab[2];
    char buf[2][32];
    int vis;
    lv_coord_t h;
} roll_t;

typedef struct gauge gauge_t;

typedef struct {
    gauge_t *g;
    int phase;
    int target_x10;
} gauge_move_t;

struct gauge {
    lv_obj_t *obj;
    lv_obj_t *arc;
    lv_img_dsc_t face_dsc;
    void *face_buf;
    lv_obj_t *value;
    lv_obj_t *unit;
    roll_t cap;
    char value_buf[12];
    char unit_buf[8];
    int shown_x10;
    int arc_shown;
    metric_t metric;
    bool hero;
    bool busy;
    const char *slot;
    gauge_move_t *move;
};

typedef struct {
    lv_obj_t *panel;
    lv_obj_t *value;
    lv_obj_t *unit;
    char buf[12];
    char unit_buf[12];
    int shown;
    tile_kind_t kind;
} tile_t;

typedef struct {
    lv_obj_t *row;
    lv_obj_t *val;
    lv_obj_t *bar;
    char buf[64];
    lv_coord_t home_x;
} sys_row_t;

typedef struct {
    lv_coord_t x, y, w, h;
} box_t;

typedef struct {
    int cpu;
    int sram_free_pct;
    int psram_used_pct;
    int temp_x10;
    bool temp_ok;
    int heap_kb;
    int psram_free_mb_x10;
    int fps;
} sample_t;

static phase_t s_phase = PHASE_INTRO;
static sample_t s_sample;
static int s_color_level;
static bool s_color_ready;
static bool s_seen_dash;
static bool s_page_busy;
static bool s_overlay_open;
static bool s_idle;
static bool s_waking;
static int s_page;
static int s_page_gen;
static int s_ov_opening = -1;
static int s_ov_token;
static tile_kind_t s_ov_kind;

static gauge_t s_main;
static gauge_t s_left;
static gauge_t s_right;
static gauge_move_t s_move[3];
static tile_t s_tiles[TILE_COUNT];
static sys_row_t s_sys[SYS_COUNT];

static lv_obj_t *s_dash;
static lv_obj_t *s_viewport;
static lv_obj_t *s_track;
static lv_obj_t *s_page_obj[UI_PAGE_COUNT];
static lv_obj_t *s_dot[UI_PAGE_COUNT];
static roll_t s_title;
static lv_obj_t *s_intro;
static lv_obj_t *s_wordmark;
static lv_obj_t *s_intro_line;
static lv_coord_t s_wordmark_y;
static lv_obj_t *s_uptime_lbl;
static lv_obj_t *s_status_dot;
static char s_uptime[16];

static lv_obj_t *s_chart;
static lv_chart_series_t *s_series;
static lv_timer_t *s_timer;
static lv_timer_t *s_io_timer;

static lv_obj_t *s_mask;
static lv_obj_t *s_detail;
static lv_obj_t *s_ov_title;
static lv_obj_t *s_ov_value;
static lv_obj_t *s_ov_unit;
static lv_obj_t *s_ov_stats;
static lv_obj_t *s_ov_chart;
static lv_chart_series_t *s_ov_series;
static char s_ov_value_buf[16];
static char s_ov_stats_buf[72];
static box_t s_ov_from;
static box_t s_ov_to;

static lv_obj_t *s_qr_card;
static lv_obj_t *s_about_lbl[4];
static lv_coord_t s_about_y[4];

static lv_obj_t *s_idle_layer;
static lv_obj_t *s_idle_clock;
static lv_obj_t *s_idle_line;

static int s_hist[TILE_COUNT][UI_CHART_POINTS];
static int s_hist_n;
static int s_hist_head;

static temperature_sensor_handle_t s_tsens;
static bool s_tsens_ok;
static bool s_cpu_primed;
static configRUN_TIME_COUNTER_TYPE s_prev_idle[UI_CPU_CORES];
static int64_t s_prev_us;

static uint32_t s_fps_frames;
static uint32_t s_fps_stamp;
static void (*s_prev_monitor)(lv_disp_drv_t *, uint32_t, uint32_t);
static uint32_t s_ms_stamp;
static uint32_t s_ms_acc;
static uint32_t s_perf_stamp;
static uint32_t s_perf_frames;
static uint32_t s_perf_ms_sum;
static uint32_t s_perf_ms_max;
static uint32_t s_perf_px_sum;

static int s_sweep_token;
static int s_down_main;
static int s_down_left;
static int s_down_right;

static const lv_style_prop_t k_press_props[] = {
    LV_STYLE_BG_COLOR,
    LV_STYLE_BG_OPA,
    LV_STYLE_BORDER_COLOR,
    0,
};
static lv_style_transition_dsc_t s_press_tr;

static const char *const k_main_caption[METRIC_COUNT] = {
    "01  " BULLET "  cpu load",
    "01  " BULLET "  sram free",
    "01  " BULLET "  psram used",
    "01  " BULLET "  chip temp",
};
static const char *const k_hero_unit[METRIC_COUNT] = {"%", "%", "%", "\xC2\xB0" "C"};
static const char *const k_side_unit[METRIC_COUNT] = {"%", "free %", "used %", "\xC2\xB0" "C"};
static const char *const k_short_name[METRIC_COUNT] = {"cpu", "sram", "psram", "temp"};
static const char *const k_page_title[UI_PAGE_COUNT] = {
    "01  " BULLET "  dash",
    "02  " BULLET "  io",
    "03  " BULLET "  i2c",
    "04  " BULLET "  can",
    "05  " BULLET "  rs485",
    "06  " BULLET "  system",
    "07  " BULLET "  about",
};
static const char *const k_tile_name[TILE_COUNT] = {"HEAP", "PSRAM", "CHIP TEMP", "FPS"};
static const char *const k_tile_unit[TILE_COUNT] = {"KB", "MB", "\xC2\xB0" "C", "fps"};
static const char *const k_sys_key[SYS_COUNT] = {
    "CHIP", "CORES", "CPU", "FLASH", "PSRAM", "MAC",
    "IDF", "LVGL", "RESET", "UPTIME", "RTC", "PERIPH", "MIN HEAP",
};

static const box_t k_ov_full = {140, 90, 520, 300};

static void sample_all(bool push_chart);
static void apply_live(void);
static void apply_color(void);
static void start_intro(void);
static void gauge_texts(gauge_t *g, bool animate);
static void start_retract_sweep(gauge_t *g);
static void start_resweep(void);
static void overlay_refresh(void);
static void overlay_open(tile_t *t);
static void overlay_close(void);
static void system_prime(void);
static void system_play(void);
static void about_prime(void);
static void about_play(void);
static void roll_set(roll_t *r, const char *text, bool animate);
static void dots_layout(int active, bool animate);
static void go_page(int dest);
static void on_dot_clicked(lv_event_t *e);
static void io_timer_cb(lv_timer_t *timer);
static void enter_idle(void);
static void refresh_minheap(void);
static int metric_target_x10(metric_t metric);

static lv_color_t hex(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static void anim_i32(void *var, lv_anim_exec_xcb_t exec, int32_t from, int32_t to,
                     uint32_t time_ms, lv_anim_path_cb_t path, uint32_t delay_ms,
                     lv_anim_ready_cb_t ready)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, time_ms);
    lv_anim_set_path_cb(&a, path ? path : lv_anim_path_linear);
    if (delay_ms) {
        lv_anim_set_delay(&a, delay_ms);
    }
    if (ready) {
        lv_anim_set_ready_cb(&a, ready);
    }
    lv_anim_start(&a);
}

static void make_plain(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

static void style_screen(lv_obj_t *scr, uint32_t bg)
{
    lv_obj_set_style_bg_color(scr, hex(bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_outline_width(scr, 0, 0);
    lv_obj_set_style_shadow_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_text_color(scr, hex(HEX_TEXT), 0);
    lv_obj_set_style_text_font(scr, &lv_font_montserrat_16, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_panel(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    make_plain(p);
    lv_obj_set_style_bg_color(p, hex(HEX_PANEL), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(p, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_border_color(p, hex(HEX_LINE), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(p, 0, 0);
    lv_obj_set_style_outline_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    return p;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, hex(color), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

static void label_static(lv_obj_t *lbl, char *buf, size_t buflen, const char *text)
{
    snprintf(buf, buflen, "%s", text);
    lv_label_set_text_static(lbl, buf);
}

static bool ui_live(void)
{
    return s_phase == PHASE_LIVE && !s_idle && !s_waking && !s_overlay_open;
}

bool bashcat_ui_page_ready(int page)
{
    return ui_live() && !s_page_busy && s_page == page;
}

static void fade_exec(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

/* Entrance fades end at COVER. A leftover local opa style forces a layer
 * on every later redraw, so drop it once the animation has landed. */
static void opa_drop(lv_anim_t *a)
{
    lv_obj_t *obj = a->var;
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    lv_obj_remove_local_style_prop(obj, LV_STYLE_OPA, 0);
}

static void text_opa_exec(void *var, int32_t v)
{
    lv_obj_set_style_text_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void x_exec(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, (lv_coord_t)v);
}

static void y_exec(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, (lv_coord_t)v);
}

static void w_exec(void *var, int32_t v)
{
    lv_obj_set_width((lv_obj_t *)var, (lv_coord_t)v);
}

static void zoom_exec(void *var, int32_t v)
{
    lv_obj_set_style_transform_zoom((lv_obj_t *)var, (lv_coord_t)v, 0);
}

static void bg_opa_exec(void *var, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void roll_init(roll_t *r, lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                      lv_coord_t w, lv_coord_t h, lv_text_align_t align)
{
    r->h = h;
    r->vis = 0;
    r->buf[0][0] = '\0';
    r->buf[1][0] = '\0';
    r->clip = lv_obj_create(parent);
    make_plain(r->clip);
    lv_obj_clear_flag(r->clip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r->clip, w, h);
    lv_obj_set_style_bg_opa(r->clip, LV_OPA_TRANSP, 0);
    for (int i = 0; i < 2; i++) {
        r->lab[i] = make_label(r->clip, font, color);
        lv_obj_set_width(r->lab[i], w);
        lv_obj_set_style_text_align(r->lab[i], align, 0);
        lv_label_set_long_mode(r->lab[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_y(r->lab[i], (i == 0) ? 0 : h);
        if (i == 1) {
            lv_obj_set_style_text_opa(r->lab[i], LV_OPA_TRANSP, 0);
        }
    }
}

static void roll_set(roll_t *r, const char *text, bool animate)
{
    if (r == NULL || r->lab[0] == NULL || text == NULL) {
        return;
    }
    if (strcmp(r->buf[r->vis], text) == 0) {
        return;
    }
    if (!animate) {
        label_static(r->lab[0], r->buf[0], sizeof r->buf[0], text);
        lv_obj_set_y(r->lab[0], 0);
        lv_obj_set_style_text_opa(r->lab[0], LV_OPA_COVER, 0);
        lv_obj_set_y(r->lab[1], r->h);
        lv_obj_set_style_text_opa(r->lab[1], LV_OPA_TRANSP, 0);
        r->buf[1][0] = '\0';
        r->vis = 0;
        return;
    }
    int neu = r->vis ^ 1;
    int old = r->vis;
    label_static(r->lab[neu], r->buf[neu], sizeof r->buf[neu], text);
    lv_anim_del(r->lab[old], y_exec);
    lv_anim_del(r->lab[old], text_opa_exec);
    lv_anim_del(r->lab[neu], y_exec);
    lv_anim_del(r->lab[neu], text_opa_exec);
    lv_obj_set_y(r->lab[old], 0);
    lv_obj_set_style_text_opa(r->lab[old], LV_OPA_COVER, 0);
    lv_obj_set_y(r->lab[neu], r->h);
    lv_obj_set_style_text_opa(r->lab[neu], LV_OPA_TRANSP, 0);
    anim_i32(r->lab[old], y_exec, 0, -(int32_t)r->h, UI_ROLL_MS, lv_anim_path_ease_out, 0, NULL);
    anim_i32(r->lab[old], text_opa_exec, LV_OPA_COVER, 0, UI_ROLL_MS, lv_anim_path_ease_out, 0, NULL);
    anim_i32(r->lab[neu], y_exec, r->h, 0, UI_ROLL_MS, lv_anim_path_ease_out, 0, NULL);
    anim_i32(r->lab[neu], text_opa_exec, 0, LV_OPA_COVER, UI_ROLL_MS, lv_anim_path_ease_out, 0, NULL);
    r->vis = neu;
}

static int clamp_pct(int v)
{
    if (v < 0) {
        return 0;
    }
    if (v > 100) {
        return 100;
    }
    return v;
}

static int level_for_cpu(int cpu)
{
    if (cpu > UI_CPU_CRIT) {
        return 2000;
    }
    if (cpu > UI_CPU_WARN) {
        return 1000;
    }
    return 0;
}

static lv_color_t color_at(int level)
{
    if (level < 0) {
        level = 0;
    }
    if (level > 2000) {
        level = 2000;
    }
    if (level <= 1000) {
        return lv_color_mix(hex(HEX_WARN), hex(HEX_ACCENT), (uint8_t)((level * 255) / 1000));
    }
    return lv_color_mix(hex(HEX_CRIT), hex(HEX_WARN), (uint8_t)(((level - 1000) * 255) / 1000));
}

static int metric_target_x10(metric_t metric)
{
    switch (metric) {
    case METRIC_SRAM:
        return s_sample.sram_free_pct * 10;
    case METRIC_PSRAM:
        return s_sample.psram_used_pct * 10;
    case METRIC_TEMP:
        return s_sample.temp_ok ? s_sample.temp_x10 : 0;
    case METRIC_CPU:
    default:
        return s_sample.cpu * 10;
    }
}

static void format_x10(char *buf, size_t buflen, int x10, bool decimal)
{
    if (!decimal) {
        int arc = x10 / 10;
        if (arc < 0) {
            arc = 0;
        }
        if (arc > 100) {
            arc = 100;
        }
        snprintf(buf, buflen, "%d", arc);
        return;
    }
    int neg = x10 < 0;
    int av = neg ? -x10 : x10;
    snprintf(buf, buflen, "%s%d.%d", neg ? "-" : "", av / 10, av % 10);
}

static void gauge_show(gauge_t *g, int x10)
{
    bool decimal = (g->metric == METRIC_TEMP);
    g->shown_x10 = x10;
    int arc = x10 / 10;
    if (arc < 0) {
        arc = 0;
    }
    if (arc > 100) {
        arc = 100;
    }
    if (g->arc && arc != g->arc_shown) {
        lv_arc_set_value(g->arc, arc);
        g->arc_shown = arc;
    }
    char next[12];
    format_x10(next, sizeof next, x10, decimal);
    if (strcmp(next, g->value_buf) != 0) {
        memcpy(g->value_buf, next, strlen(next) + 1);
        lv_label_set_text_static(g->value, g->value_buf);
    }
}

static void gauge_exec(void *var, int32_t v)
{
    gauge_show((gauge_t *)var, (int)v);
}

static void color_exec(void *var, int32_t v)
{
    (void)var;
    s_color_level = (int)v;
    s_color_ready = true;
    lv_color_t c = color_at(s_color_level);
    if (s_main.arc) {
        lv_obj_set_style_arc_color(s_main.arc, c, LV_PART_INDICATOR);
    }
    if (s_main.value) {
        lv_obj_set_style_text_color(s_main.value, c, 0);
    }
}

static void apply_color(void)
{
    int level = (s_main.metric == METRIC_CPU) ? level_for_cpu(s_sample.cpu) : 0;
    if (s_color_ready && level == s_color_level) {
        return;
    }
    anim_i32(&s_color_level, color_exec, s_color_level, level, UI_COLOR_ANIM_MS,
             lv_anim_path_ease_out, 0, NULL);
}

static void gauge_texts(gauge_t *g, bool animate)
{
    char cap[32];
    if (g->hero) {
        snprintf(cap, sizeof cap, "%s", k_main_caption[g->metric]);
    } else {
        snprintf(cap, sizeof cap, "%s  " BULLET "  %s", g->slot ? g->slot : "00", k_short_name[g->metric]);
    }
    roll_set(&g->cap, cap, animate);
    label_static(g->unit, g->unit_buf, sizeof g->unit_buf,
                 g->hero ? k_hero_unit[g->metric] : k_side_unit[g->metric]);
}

static void move_exec(void *var, int32_t v)
{
    gauge_move_t *m = var;
    if (m->g) {
        gauge_show(m->g, (int)v);
    }
}

static void move_ready(lv_anim_t *a)
{
    gauge_move_t *m = a->var;
    if (m->phase == 0) {
        m->phase = 1;
        anim_i32(m, move_exec, 0, m->target_x10, UI_SWEEP_TO_MS, lv_anim_path_ease_out, 0, move_ready);
        return;
    }
    if (m->g) {
        m->g->busy = false;
    }
}

static void start_retract_sweep(gauge_t *g)
{
    if (g->move == NULL) {
        return;
    }
    gauge_move_t *m = g->move;
    lv_anim_del(m, move_exec);
    lv_anim_del(g, gauge_exec);
    m->g = g;
    m->target_x10 = metric_target_x10(g->metric);
    g->busy = true;
    int from = g->shown_x10;
    if (from < 0) {
        from = 0;
    }
    if (from == 0) {
        m->phase = 1;
        anim_i32(m, move_exec, 0, m->target_x10, UI_SWEEP_TO_MS, lv_anim_path_ease_out, 0, move_ready);
        return;
    }
    m->phase = 0;
    anim_i32(m, move_exec, from, 0, UI_RETRACT_MS, lv_anim_path_ease_in, 0, move_ready);
}

static void start_resweep_one(gauge_t *g)
{
    if (g->move == NULL) {
        return;
    }
    gauge_move_t *m = g->move;
    lv_anim_del(m, move_exec);
    lv_anim_del(g, gauge_exec);
    gauge_show(g, 0);
    m->g = g;
    m->phase = 1;
    m->target_x10 = metric_target_x10(g->metric);
    g->busy = true;
    anim_i32(m, move_exec, 0, m->target_x10, UI_RESWEEP_MS, lv_anim_path_ease_out, 0, move_ready);
}

static void start_resweep(void)
{
    if (!s_seen_dash) {
        return;
    }
    start_resweep_one(&s_main);
    start_resweep_one(&s_left);
    start_resweep_one(&s_right);
}

/* ---- sensors ---- */

static void sensors_init(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&cfg, &s_tsens);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_UI, "temp sensor install failed: %s", esp_err_to_name(err));
        s_tsens_ok = false;
        return;
    }
    err = temperature_sensor_enable(s_tsens);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_UI, "temp sensor enable failed: %s", esp_err_to_name(err));
        s_tsens_ok = false;
        return;
    }
    s_tsens_ok = true;
}

static void cpu_reprime(void)
{
    s_cpu_primed = false;
}

static bool sample_cpu(void)
{
    int64_t now = esp_timer_get_time();
    configRUN_TIME_COUNTER_TYPE idle[UI_CPU_CORES];
    int cores = portNUM_PROCESSORS;
    if (cores > UI_CPU_CORES) {
        cores = UI_CPU_CORES;
    }
    for (int i = 0; i < cores; i++) {
        idle[i] = ulTaskGetIdleRunTimeCounterForCore((BaseType_t)i);
    }
    if (!s_cpu_primed) {
        s_prev_us = now;
        for (int i = 0; i < cores; i++) {
            s_prev_idle[i] = idle[i];
        }
        s_cpu_primed = true;
        return false;
    }
    int64_t dt = now - s_prev_us;
    uint64_t idle_delta = 0;
    for (int i = 0; i < cores; i++) {
        idle_delta += (uint32_t)(idle[i] - s_prev_idle[i]);
        s_prev_idle[i] = idle[i];
    }
    s_prev_us = now;
    if (dt <= 0) {
        return false;
    }
    uint64_t total = (uint64_t)dt * (uint64_t)cores;
    if (idle_delta > total) {
        idle_delta = total;
    }
    int load = (int)(100ULL - (idle_delta * 100ULL) / total);
    s_sample.cpu = clamp_pct(load);
    return true;
}

static void sample_mem(void)
{
    size_t free_i = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t tot_i = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t free_p = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t tot_p = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    s_sample.heap_kb = (int)(free_i / 1024U);
    s_sample.sram_free_pct = tot_i ? (int)((free_i * 100ULL) / tot_i) : 0;
    s_sample.sram_free_pct = clamp_pct(s_sample.sram_free_pct);
    s_sample.psram_free_mb_x10 = (int)((free_p * 10ULL) / (1024ULL * 1024ULL));
    if (tot_p) {
        size_t used = (free_p < tot_p) ? (tot_p - free_p) : 0;
        s_sample.psram_used_pct = clamp_pct((int)((used * 100ULL) / tot_p));
    } else {
        s_sample.psram_used_pct = 0;
    }
}

static void sample_temp(void)
{
    if (!s_tsens_ok) {
        s_sample.temp_ok = false;
        return;
    }
    float c = 0.0f;
    if (temperature_sensor_get_celsius(s_tsens, &c) != ESP_OK) {
        s_sample.temp_ok = false;
        return;
    }
    if (c >= 0.0f) {
        s_sample.temp_x10 = (int)(c * 10.0f + 0.5f);
    } else {
        s_sample.temp_x10 = (int)(c * 10.0f - 0.5f);
    }
    s_sample.temp_ok = true;
}

static void fps_ms_show(uint32_t ms)
{
    tile_t *t = &s_tiles[TILE_FPS];
    if (t->unit == NULL) {
        return;
    }
    char next[12];
    snprintf(next, sizeof next, "%u ms", (unsigned)ms);
    if (strcmp(next, t->unit_buf) == 0) {
        return;
    }
    memcpy(t->unit_buf, next, strlen(next) + 1);
    lv_label_set_text_static(t->unit, t->unit_buf);
}

static void perf_roll(void)
{
    if (lv_tick_elaps(s_ms_stamp) >= 1000) {
        uint32_t show = s_ms_acc;
        s_ms_acc = 0;
        s_ms_stamp = lv_tick_get();
        fps_ms_show(show);
    }
    if (lv_tick_elaps(s_perf_stamp) >= 2000) {
        uint32_t elaps = lv_tick_elaps(s_perf_stamp);
        uint32_t frames = s_perf_frames;
        uint32_t fps = elaps ? (uint32_t)((frames * 1000ULL) / elaps) : 0;
        uint32_t avg_ms = frames ? s_perf_ms_sum / frames : 0;
        uint32_t avg_px = frames ? s_perf_px_sum / frames : 0;
        ESP_LOGI("perf", "fps=%u avg_ms=%u max_ms=%u avg_px=%u", (unsigned)fps, (unsigned)avg_ms,
                 (unsigned)s_perf_ms_max, (unsigned)avg_px);
        s_perf_frames = 0;
        s_perf_ms_sum = 0;
        s_perf_ms_max = 0;
        s_perf_px_sum = 0;
        s_perf_stamp = lv_tick_get();
    }
}

static void fps_monitor_cb(lv_disp_drv_t *drv, uint32_t time_ms, uint32_t px)
{
    s_fps_frames++;
    if (time_ms > s_ms_acc) {
        s_ms_acc = time_ms;
    }
    s_perf_frames++;
    s_perf_ms_sum += time_ms;
    if (time_ms > s_perf_ms_max) {
        s_perf_ms_max = time_ms;
    }
    s_perf_px_sum += px;
    if (s_prev_monitor) {
        s_prev_monitor(drv, time_ms, px);
    }
}

static void fps_reset(void)
{
    s_fps_frames = 0;
    s_fps_stamp = lv_tick_get();
}

static void sample_fps(void)
{
    uint32_t elaps = lv_tick_elaps(s_fps_stamp);
    if (elaps == 0) {
        return;
    }
    s_sample.fps = (int)((s_fps_frames * 1000ULL) / elaps);
    if (s_sample.fps < 0) {
        s_sample.fps = 0;
    }
    if (s_sample.fps > 999) {
        s_sample.fps = 999;
    }
    s_fps_frames = 0;
    s_fps_stamp = lv_tick_get();
}

static void install_fps_monitor(void)
{
    lv_disp_t *disp = lv_disp_get_default();
    if (disp == NULL || disp->driver == NULL) {
        return;
    }
    s_prev_monitor = disp->driver->monitor_cb;
    disp->driver->monitor_cb = fps_monitor_cb;
    s_ms_stamp = lv_tick_get();
    s_perf_stamp = s_ms_stamp;
    fps_reset();
}

static void update_uptime(void)
{
    int64_t us = esp_timer_get_time();
    if (us < 0) {
        us = 0;
    }
    uint32_t sec = (uint32_t)(us / 1000000LL);
    uint32_t h = sec / 3600U;
    if (h > 99U) {
        h = 99U;
    }
    char buf[16];
    snprintf(buf, sizeof buf, "T+%02u:%02u:%02u", (unsigned)h, (unsigned)((sec / 60U) % 60U),
             (unsigned)(sec % 60U));
    if (strcmp(buf, s_uptime) == 0) {
        return;
    }
    memcpy(s_uptime, buf, strlen(buf) + 1);
    if (s_uptime_lbl) {
        lv_label_set_text_static(s_uptime_lbl, s_uptime);
    }
    if (s_idle_clock) {
        lv_label_set_text_static(s_idle_clock, s_uptime);
    }
    if (s_sys[SYS_UPTIME].val) {
        lv_label_set_text_static(s_sys[SYS_UPTIME].val, s_uptime);
    }
}

static void format_kind(tile_kind_t kind, int v, char *buf, size_t buflen)
{
    switch (kind) {
    case TILE_PSRAM:
        snprintf(buf, buflen, "%d.%d", v / 10, v < 0 ? 0 : v % 10);
        break;
    case TILE_TEMP:
        if (!s_sample.temp_ok && v == 0) {
            snprintf(buf, buflen, "--");
        } else {
            format_x10(buf, buflen, v, true);
        }
        break;
    case TILE_HEAP:
    case TILE_FPS:
    default:
        snprintf(buf, buflen, "%d", v);
        break;
    }
}

static void tile_exec(void *var, int32_t v)
{
    tile_t *t = (tile_t *)var;
    t->shown = (int)v;
    char next[12];
    format_kind(t->kind, t->shown, next, sizeof next);
    if (strcmp(next, t->buf) != 0) {
        memcpy(t->buf, next, strlen(next) + 1);
        lv_label_set_text_static(t->value, t->buf);
    }
}

static int tile_target(tile_kind_t kind)
{
    switch (kind) {
    case TILE_HEAP:
        return s_sample.heap_kb;
    case TILE_PSRAM:
        return s_sample.psram_free_mb_x10;
    case TILE_TEMP:
        return s_sample.temp_ok ? s_sample.temp_x10 : 0;
    case TILE_FPS:
    default:
        return s_sample.fps;
    }
}

static void anim_tile(tile_t *t, int target)
{
    if (t->shown == target) {
        tile_exec(t, target);
        return;
    }
    anim_i32(t, tile_exec, t->shown, target, UI_VALUE_ANIM_MS, lv_anim_path_ease_out, 0, NULL);
}

static void anim_gauge_live(gauge_t *g, int target_x10)
{
    if (s_phase != PHASE_LIVE || g->busy) {
        return;
    }
    if (g->shown_x10 == target_x10) {
        return;
    }
    anim_i32(g, gauge_exec, g->shown_x10, target_x10, UI_VALUE_ANIM_MS, lv_anim_path_ease_out, 0, NULL);
}

static void apply_live(void)
{
    if (s_phase != PHASE_LIVE) {
        return;
    }
    anim_gauge_live(&s_main, metric_target_x10(s_main.metric));
    anim_gauge_live(&s_left, metric_target_x10(s_left.metric));
    anim_gauge_live(&s_right, metric_target_x10(s_right.metric));
    apply_color();
    for (int i = 0; i < TILE_COUNT; i++) {
        anim_tile(&s_tiles[i], tile_target(s_tiles[i].kind));
    }
}

static void push_history(void)
{
    for (int k = 0; k < TILE_COUNT; k++) {
        s_hist[k][s_hist_head] = tile_target((tile_kind_t)k);
    }
    s_hist_head++;
    if (s_hist_head >= UI_CHART_POINTS) {
        s_hist_head = 0;
    }
    if (s_hist_n < UI_CHART_POINTS) {
        s_hist_n++;
    }
}

static int hist_at(tile_kind_t kind, int oldest_i)
{
    int start = s_hist_head - s_hist_n;
    while (start < 0) {
        start += UI_CHART_POINTS;
    }
    return s_hist[kind][(start + oldest_i) % UI_CHART_POINTS];
}

static void overlay_refresh(void)
{
    if (!s_overlay_open || s_ov_value == NULL) {
        return;
    }
    tile_kind_t k = s_ov_kind;
    format_kind(k, tile_target(k), s_ov_value_buf, sizeof s_ov_value_buf);
    lv_label_set_text_static(s_ov_value, s_ov_value_buf);
    if (s_hist_n <= 0) {
        snprintf(s_ov_stats_buf, sizeof s_ov_stats_buf, "min --    max --    avg --");
        lv_label_set_text_static(s_ov_stats, s_ov_stats_buf);
        return;
    }
    int lo = hist_at(k, 0);
    int hi = lo;
    long sum = 0;
    for (int i = 0; i < s_hist_n; i++) {
        int v = hist_at(k, i);
        if (v < lo) {
            lo = v;
        }
        if (v > hi) {
            hi = v;
        }
        sum += v;
    }
    int avg = (int)(sum / s_hist_n);
    char a[12], b[12], c[12];
    format_kind(k, lo, a, sizeof a);
    format_kind(k, hi, b, sizeof b);
    format_kind(k, avg, c, sizeof c);
    snprintf(s_ov_stats_buf, sizeof s_ov_stats_buf, "min %s    max %s    avg %s", a, b, c);
    lv_label_set_text_static(s_ov_stats, s_ov_stats_buf);
    if (s_ov_chart == NULL || s_ov_series == NULL) {
        return;
    }
    int pad = (hi - lo) / 8;
    if (pad < 1) {
        pad = 1;
    }
    int y0 = lo - pad;
    int y1 = hi + pad;
    if (y1 <= y0) {
        y1 = y0 + 1;
    }
    lv_chart_set_range(s_ov_chart, LV_CHART_AXIS_PRIMARY_Y, y0, y1);
    for (int i = 0; i < UI_CHART_POINTS; i++) {
        lv_coord_t v = LV_CHART_POINT_NONE;
        if (i < s_hist_n) {
            v = (lv_coord_t)hist_at(k, i);
        }
        lv_chart_set_value_by_id(s_ov_chart, s_ov_series, (uint16_t)i, v);
    }
    lv_chart_refresh(s_ov_chart);
}

static int minheap_pct(uint32_t mn)
{
    size_t internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t denom = internal;
    if (mn > internal) {
        denom = internal + psram;
    }
    if (denom == 0) {
        return 0;
    }
    int pct = (int)((mn * 100ull) / denom);
    if (pct < 0) {
        return 0;
    }
    if (pct > 100) {
        return 100;
    }
    return pct;
}

static void refresh_minheap(void)
{
    sys_row_t *r = &s_sys[SYS_MINHEAP];
    if (r->val == NULL) {
        return;
    }
    uint32_t mn = esp_get_minimum_free_heap_size();
    char next[32];
    if (mn >= 1024u * 1024u) {
        uint32_t x10 = (uint32_t)((mn * 10ull) / (1024ull * 1024ull));
        snprintf(next, sizeof next, "%u.%u MB", (unsigned)(x10 / 10u), (unsigned)(x10 % 10u));
    } else {
        snprintf(next, sizeof next, "%u KB", (unsigned)(mn / 1024u));
    }
    if (strcmp(next, r->buf) != 0) {
        memcpy(r->buf, next, strlen(next) + 1);
        lv_label_set_text_static(r->val, r->buf);
    }
    if (r->bar) {
        lv_coord_t tw = lv_obj_get_width(lv_obj_get_parent(r->bar));
        lv_obj_set_width(r->bar, (lv_coord_t)((tw * minheap_pct(mn)) / 100));
    }
}

static void sample_all(bool push_chart)
{
    bool cpu_ok = sample_cpu();
    sample_mem();
    sample_temp();
    sample_fps();
    update_uptime();
    refresh_minheap();
    push_history();
    if (s_overlay_open) {
        overlay_refresh();
    }
    /* The slide already repaints the whole screen. Keep the sample, and
     * wait until the track settles before starting value anims or
     * invalidating the sparkline. */
    if (s_page_busy) {
        return;
    }
    if (push_chart && cpu_ok && s_chart && s_series) {
        lv_chart_set_next_value(s_chart, s_series, (lv_coord_t)s_sample.cpu);
    }
    if (s_phase == PHASE_LIVE) {
        apply_live();
    } else if (s_phase == PHASE_SWEEP) {
        for (int i = 0; i < TILE_COUNT; i++) {
            anim_tile(&s_tiles[i], tile_target(s_tiles[i].kind));
        }
    }
}

static void sample_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    sample_all(true);
    perf_roll();
    if (s_phase == PHASE_LIVE && !s_idle && !s_waking && !s_overlay_open && !s_page_busy) {
        if (lv_disp_get_inactive_time(NULL) >= UI_IDLE_TIMEOUT_MS) {
            enter_idle();
        }
    }
}

/* ---- gauges ---- */

static void init_press_style(void)
{
    lv_style_transition_dsc_init(&s_press_tr, k_press_props, lv_anim_path_ease_out, UI_PRESS_MS, 0, NULL);
}

/* Ticks, tick numbers, and the gray track are drawn once into PSRAM.
 * The buffer stays for the life of the UI. Do not lv_snapshot_free() it. */
static void gauge_take_face(gauge_t *g, lv_obj_t *meter, lv_coord_t size)
{
    lv_obj_update_layout(meter);
    uint32_t need = lv_snapshot_buf_size_needed(meter, LV_IMG_CF_TRUE_COLOR_ALPHA);
    if (need == 0) {
        ESP_LOGE(TAG_UI, "gauge face size 0");
        return;
    }
    void *buf = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG_UI, "gauge face alloc %u failed", (unsigned)need);
        return;
    }
    if (lv_snapshot_take_to_buf(meter, LV_IMG_CF_TRUE_COLOR_ALPHA, &g->face_dsc, buf, need) != LV_RES_OK) {
        ESP_LOGE(TAG_UI, "gauge face snapshot failed");
        heap_caps_free(buf);
        return;
    }
    g->face_buf = buf;
    lv_obj_t *img = lv_img_create(g->obj);
    make_plain(img);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_img_set_src(img, &g->face_dsc);
    lv_coord_t ext = 0;
    if (g->face_dsc.header.w > (uint32_t)size) {
        ext = (lv_coord_t)((g->face_dsc.header.w - (uint32_t)size) / 2u);
    }
    lv_obj_set_pos(img, (lv_coord_t)(-ext), (lv_coord_t)(-ext));
}

static void build_gauge(gauge_t *g, lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t size,
                        int ticks, int major_nth, int minor_len, int major_len, bool hero)
{
    g->hero = hero;
    g->obj = lv_obj_create(parent);
    make_plain(g->obj);
    lv_obj_set_pos(g->obj, x, y);
    lv_obj_set_size(g->obj, size, size);
    lv_obj_set_style_bg_opa(g->obj, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(g->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(g->obj, 12, 0);
    lv_obj_set_style_bg_color(g->obj, hex(HEX_BG), 0);
    lv_obj_set_style_bg_color(g->obj, hex(HEX_PRESS), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(g->obj, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_transition(g->obj, &s_press_tr, 0);
    lv_obj_set_style_transition(g->obj, &s_press_tr, LV_STATE_PRESSED);

    int pad = hero ? 12 : 8;
    int width = hero ? 8 : 5;
    /* r_mod is -(major_len + 2) on both gauges. The arc's outer radius is
     * the meter's content radius plus that. lv_arc's radius is the same
     * outer edge, so the widget diameter is twice that radius. */
    int r_mod = -(major_len + 2);
    int r_out = (size - pad * 2) / 2;
    int radius = r_out + r_mod;
    int arc_size = radius * 2;
    int arc_xy = (size - arc_size) / 2;

    lv_obj_t *meter = lv_meter_create(g->obj);
    make_plain(meter);
    lv_obj_clear_flag(meter, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(meter, 0, 0);
    lv_obj_set_size(meter, size, size);
    lv_obj_set_style_bg_opa(meter, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(meter, pad, 0);
    lv_obj_set_style_text_font(meter, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_font(meter, &lv_font_montserrat_12, LV_PART_TICKS);
    lv_obj_set_style_text_color(meter, hex(HEX_MUTED), LV_PART_TICKS);
    lv_obj_set_style_line_rounded(meter, false, LV_PART_TICKS);
    lv_obj_set_style_arc_rounded(meter, false, LV_PART_ITEMS);

    lv_meter_scale_t *scale = lv_meter_add_scale(meter);
    lv_meter_set_scale_ticks(meter, scale, ticks, hero ? 2 : 1, minor_len, hex(HEX_LINE));
    /* Labels sit inside the arc band (major_len + arc width + gap) so they never overlap it. */
    lv_meter_set_scale_major_ticks(meter, scale, major_nth, 2, major_len, hex(HEX_DIM),
                                   hero ? 20 : 6);
    lv_meter_set_scale_range(meter, scale, 0, 100, 270, 135);
    if (!hero) {
        /* Side gauges: ticks only, the big number carries the value. */
        lv_obj_set_style_text_opa(meter, LV_OPA_TRANSP, LV_PART_TICKS);
    }

    lv_meter_indicator_t *track = lv_meter_add_arc(meter, scale, width, hex(HEX_LINE), r_mod);
    lv_meter_set_indicator_start_value(meter, track, 0);
    lv_meter_set_indicator_end_value(meter, track, 100);
    gauge_take_face(g, meter, size);
    lv_obj_del(meter);

    g->arc = lv_arc_create(g->obj);
    make_plain(g->arc);
    lv_obj_clear_flag(g->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(g->arc, (lv_coord_t)arc_xy, (lv_coord_t)arc_xy);
    lv_obj_set_size(g->arc, (lv_coord_t)arc_size, (lv_coord_t)arc_size);
    lv_obj_set_style_pad_all(g->arc, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g->arc, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g->arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g->arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(g->arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g->arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g->arc, false, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g->arc, hex(HEX_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(g->arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g->arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g->arc, 0, LV_PART_KNOB);
    lv_arc_set_rotation(g->arc, 0);
    lv_arc_set_mode(g->arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_bg_angles(g->arc, 135, 45);
    lv_arc_set_range(g->arc, 0, 100);
    lv_arc_set_value(g->arc, 0);
    g->arc_shown = 0;
    g->shown_x10 = 0;

    g->value = make_label(g->obj, hero ? &lv_font_montserrat_48 : &lv_font_montserrat_24, HEX_ACCENT);
    lv_obj_set_width(g->value, hero ? 180 : 90);
    lv_obj_set_style_text_align(g->value, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g->value, LV_LABEL_LONG_CLIP);
    snprintf(g->value_buf, sizeof g->value_buf, "0");
    lv_label_set_text_static(g->value, g->value_buf);
    lv_obj_align(g->value, LV_ALIGN_CENTER, 0, hero ? -8 : -2);

    g->unit = make_label(g->obj, &lv_font_montserrat_12, HEX_MUTED);
    lv_obj_set_style_text_align(g->unit, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g->unit, LV_ALIGN_CENTER, 0, hero ? 28 : 18);

    roll_init(&g->cap, g->obj, &lv_font_montserrat_12, HEX_MUTED, hero ? 220 : 148, 16,
              LV_TEXT_ALIGN_CENTER);
    /* The caption used to be a child of the padded meter, so BOTTOM_MID sat on
     * the content edge (pad px inside the widget). The container has no pad. */
    lv_obj_align(g->cap.clip, LV_ALIGN_BOTTOM_MID, 0, (lv_coord_t)((hero ? -2 : 2) - pad));
}

static void on_gauge_clicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!ui_live() || s_page_busy || s_page != 0) {
        return;
    }
    gauge_t *g = lv_event_get_user_data(e);
    if (g == NULL) {
        return;
    }
    if (g == &s_main) {
        g->metric = (metric_t)((g->metric + 1) % METRIC_COUNT);
        gauge_texts(g, true);
        start_retract_sweep(g);
        apply_color();
        return;
    }
    metric_t tmp = s_main.metric;
    s_main.metric = g->metric;
    g->metric = tmp;
    gauge_texts(&s_main, true);
    gauge_texts(g, true);
    start_retract_sweep(&s_main);
    start_retract_sweep(g);
    apply_color();
}

static void on_tile_clicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!ui_live() || s_page_busy || s_page != 0) {
        return;
    }
    overlay_open(lv_event_get_user_data(e));
}

static void build_tile(tile_t *t, lv_obj_t *parent, lv_coord_t x, lv_coord_t y, tile_kind_t kind,
                       const char *name, const char *unit)
{
    t->kind = kind;
    t->shown = 0;
    t->panel = make_panel(parent);
    lv_obj_set_pos(t->panel, x, y);
    lv_obj_set_size(t->panel, 100, 104);
    lv_obj_set_style_opa(t->panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(t->panel, hex(HEX_PRESS), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(t->panel, hex(HEX_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_transition(t->panel, &s_press_tr, 0);
    lv_obj_set_style_transition(t->panel, &s_press_tr, LV_STATE_PRESSED);
    lv_obj_add_flag(t->panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(t->panel, on_tile_clicked, LV_EVENT_CLICKED, t);

    lv_obj_t *title = make_label(t->panel, &lv_font_montserrat_12, HEX_MUTED);
    lv_label_set_text(title, name);
    lv_obj_set_width(title, 84);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    t->value = make_label(t->panel, &lv_font_montserrat_20, HEX_TEXT);
    lv_obj_set_width(t->value, 90);
    lv_obj_set_style_text_align(t->value, LV_TEXT_ALIGN_CENTER, 0);
    snprintf(t->buf, sizeof t->buf, "0");
    lv_label_set_text_static(t->value, t->buf);
    lv_obj_align(t->value, LV_ALIGN_CENTER, 0, 4);

    t->unit = make_label(t->panel, &lv_font_montserrat_12, HEX_MUTED);
    if (kind == TILE_FPS) {
        snprintf(t->unit_buf, sizeof t->unit_buf, "0 ms");
        lv_label_set_text_static(t->unit, t->unit_buf);
    } else {
        lv_label_set_text(t->unit, unit);
    }
    lv_obj_align(t->unit, LV_ALIGN_BOTTOM_MID, 0, -12);
}

static void breath_exec(void *var, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void start_breath(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_status_dot);
    lv_anim_set_exec_cb(&a, breath_exec);
    lv_anim_set_values(&a, 110, LV_OPA_COVER);
    lv_anim_set_time(&a, UI_BREATH_HALF_MS);
    lv_anim_set_playback_time(&a, UI_BREATH_HALF_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void start_idle_line(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_idle_line);
    lv_anim_set_exec_cb(&a, breath_exec);
    lv_anim_set_values(&a, 70, LV_OPA_COVER);
    lv_anim_set_time(&a, UI_IDLE_LINE_HALF_MS);
    lv_anim_set_playback_time(&a, UI_IDLE_LINE_HALF_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void stagger_fade(lv_obj_t *obj, uint32_t index)
{
    anim_i32(obj, fade_exec, LV_OPA_TRANSP, LV_OPA_COVER, UI_TILE_FADE_MS, lv_anim_path_ease_out,
             index * UI_TILE_STAGGER_MS, opa_drop);
}

/* ---- sweep ---- */

static void sweep_up_exec(void *var, int32_t v)
{
    (void)var;
    gauge_show(&s_main, (int)v * 10);
    gauge_show(&s_left, (int)v * 10);
    gauge_show(&s_right, (int)v * 10);
}

static void sweep_down_exec(void *var, int32_t v)
{
    (void)var;
    int main_v = 1000 + ((s_down_main - 1000) * (int)v) / 1000;
    int left_v = 1000 + ((s_down_left - 1000) * (int)v) / 1000;
    int right_v = 1000 + ((s_down_right - 1000) * (int)v) / 1000;
    gauge_show(&s_main, main_v);
    gauge_show(&s_left, left_v);
    gauge_show(&s_right, right_v);
}

static void sweep_down_ready(lv_anim_t *a)
{
    (void)a;
    s_phase = PHASE_LIVE;
    sample_all(true);
    s_seen_dash = true;
}

static void sweep_up_ready(lv_anim_t *a)
{
    (void)a;
    sample_all(true);
    s_down_main = metric_target_x10(s_main.metric);
    s_down_left = metric_target_x10(s_left.metric);
    s_down_right = metric_target_x10(s_right.metric);
    anim_i32(&s_sweep_token, sweep_down_exec, 0, 1000, UI_SWEEP_DOWN_MS, lv_anim_path_ease_in_out, 0,
             sweep_down_ready);
}

static void start_sweep(void)
{
    s_phase = PHASE_SWEEP;
    anim_i32(&s_sweep_token, sweep_up_exec, 0, 100, UI_SWEEP_UP_MS, lv_anim_path_ease_in_out, 0, sweep_up_ready);
}

/* ---- pages ---- */

static void dots_layout(int active, bool animate)
{
    lv_coord_t xs[UI_PAGE_COUNT];
    lv_coord_t ws[UI_PAGE_COUNT];
    lv_coord_t total = UI_DOT_GAP * (UI_PAGE_COUNT - 1);
    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        ws[i] = (i == active) ? UI_DOT_ON_W : UI_DOT_OFF_W;
        total += ws[i];
    }
    lv_coord_t x = (UI_W - total) / 2;
    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        xs[i] = x;
        x = (lv_coord_t)(x + ws[i] + UI_DOT_GAP);
        lv_obj_set_style_bg_color(s_dot[i], hex(i == active ? HEX_ACCENT : HEX_MUTED), 0);
        if (!animate) {
            lv_anim_del(s_dot[i], x_exec);
            lv_anim_del(s_dot[i], w_exec);
            lv_obj_set_x(s_dot[i], xs[i]);
            lv_obj_set_width(s_dot[i], ws[i]);
        } else {
            anim_i32(s_dot[i], x_exec, lv_obj_get_x(s_dot[i]), xs[i], UI_PAGE_MS,
                     lv_anim_path_ease_out, 0, NULL);
            anim_i32(s_dot[i], w_exec, lv_obj_get_width(s_dot[i]), ws[i], UI_PAGE_MS,
                     lv_anim_path_ease_out, 0, NULL);
        }
    }
}

static void system_prime(void)
{
    for (int i = 0; i < SYS_COUNT; i++) {
        lv_anim_del(s_sys[i].row, x_exec);
        lv_anim_del(s_sys[i].row, fade_exec);
        lv_obj_set_x(s_sys[i].row, (lv_coord_t)(s_sys[i].home_x + UI_PAGE_SLIDE_PX));
        lv_obj_set_style_opa(s_sys[i].row, LV_OPA_TRANSP, 0);
    }
}

static void system_play(void)
{
    for (int i = 0; i < SYS_COUNT; i++) {
        lv_coord_t home = s_sys[i].home_x;
        uint32_t delay = (uint32_t)i * UI_PAGE_STAGGER_MS;
        anim_i32(s_sys[i].row, x_exec, home + UI_PAGE_SLIDE_PX, home, UI_ENTER_MS,
                 lv_anim_path_ease_out, delay, NULL);
        anim_i32(s_sys[i].row, fade_exec, 0, LV_OPA_COVER, UI_ENTER_MS,
                 lv_anim_path_ease_out, delay, opa_drop);
    }
}

static void about_prime(void)
{
    lv_anim_del(s_qr_card, zoom_exec);
    lv_anim_del(s_qr_card, fade_exec);
    lv_obj_set_style_transform_zoom(s_qr_card, UI_ABOUT_ZOOM_START, 0);
    lv_obj_set_style_opa(s_qr_card, LV_OPA_TRANSP, 0);
    for (int i = 0; i < 4; i++) {
        lv_anim_del(s_about_lbl[i], y_exec);
        lv_anim_del(s_about_lbl[i], fade_exec);
        lv_obj_set_y(s_about_lbl[i], (lv_coord_t)(s_about_y[i] + UI_PAGE_SLIDE_PX));
        lv_obj_set_style_opa(s_about_lbl[i], LV_OPA_TRANSP, 0);
    }
}

static void about_play(void)
{
    anim_i32(s_qr_card, zoom_exec, UI_ABOUT_ZOOM_START, LV_IMG_ZOOM_NONE, UI_PAGE_MS,
             lv_anim_path_ease_out, 0, NULL);
    anim_i32(s_qr_card, fade_exec, 0, LV_OPA_COVER, UI_PAGE_MS, lv_anim_path_ease_out, 0, opa_drop);
    for (int i = 0; i < 4; i++) {
        uint32_t delay = (uint32_t)i * UI_PAGE_STAGGER_MS;
        lv_coord_t home = s_about_y[i];
        anim_i32(s_about_lbl[i], y_exec, home + UI_PAGE_SLIDE_PX, home, UI_ENTER_MS,
                 lv_anim_path_ease_out, delay, NULL);
        anim_i32(s_about_lbl[i], fade_exec, 0, LV_OPA_COVER, UI_ENTER_MS,
                 lv_anim_path_ease_out, delay, opa_drop);
    }
}

static void page_settled(void)
{
    s_page_busy = false;
    if (s_page == 0) {
        start_resweep();
    }
}

static void page_anim_ready(lv_anim_t *a)
{
    int gen = (int)(intptr_t)a->user_data;
    if (gen != s_page_gen) {
        return;
    }
    page_settled();
}

static void go_page(int dest)
{
    if (dest < 0 || dest >= UI_PAGE_COUNT) {
        return;
    }
    if (dest == s_page) {
        return;
    }
    s_page = dest;
    s_page_gen++;
    s_page_busy = true;
    if (dest == BASHCAT_PAGE_SYS) {
        system_prime();
        system_play();
    } else if (dest == BASHCAT_PAGE_ABOUT) {
        about_prime();
        about_play();
    } else if (dest >= BASHCAT_PAGE_IO && dest <= BASHCAT_PAGE_RS485) {
        bashcat_ui_v3_enter(dest);
    }
    roll_set(&s_title, k_page_title[dest], true);
    dots_layout(dest, true);

    lv_coord_t from = lv_obj_get_x(s_track);
    lv_coord_t to = (lv_coord_t)(-dest * UI_W);
    lv_anim_del(s_track, x_exec);
    if (from == to) {
        page_settled();
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_track);
    lv_anim_set_exec_cb(&a, x_exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, UI_TRACK_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_user_data(&a, (void *)(intptr_t)s_page_gen);
    lv_anim_set_ready_cb(&a, page_anim_ready);
    lv_anim_start(&a);
}

static void on_gesture(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) {
        return;
    }
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL) {
        return;
    }
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    lv_indev_wait_release(indev);
    if (!ui_live()) {
        return;
    }
    if (dir == LV_DIR_LEFT) {
        go_page(s_page + 1);
    } else if (dir == LV_DIR_RIGHT) {
        go_page(s_page - 1);
    }
}

static void on_dot_clicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!ui_live() || s_page_busy) {
        return;
    }
    go_page((int)(intptr_t)lv_event_get_user_data(e));
}

/* ---- overlay ---- */

static lv_coord_t lerp_c(lv_coord_t a, lv_coord_t b, int32_t t)
{
    return (lv_coord_t)(a + (int32_t)(b - a) * t / 1000);
}

static box_t box_of(lv_obj_t *obj)
{
    box_t b;
    b.x = lv_obj_get_x(obj);
    b.y = lv_obj_get_y(obj);
    b.w = lv_obj_get_width(obj);
    b.h = lv_obj_get_height(obj);
    return b;
}

static box_t box_screen(lv_obj_t *obj)
{
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    box_t b;
    b.x = a.x1;
    b.y = a.y1;
    b.w = (lv_coord_t)(a.x2 - a.x1 + 1);
    b.h = (lv_coord_t)(a.y2 - a.y1 + 1);
    return b;
}

static void ov_exec(void *var, int32_t v)
{
    (void)var;
    box_t b;
    b.x = lerp_c(s_ov_from.x, s_ov_to.x, v);
    b.y = lerp_c(s_ov_from.y, s_ov_to.y, v);
    b.w = lerp_c(s_ov_from.w, s_ov_to.w, v);
    b.h = lerp_c(s_ov_from.h, s_ov_to.h, v);
    if (b.w < 1) {
        b.w = 1;
    }
    if (b.h < 1) {
        b.h = 1;
    }
    lv_obj_set_pos(s_detail, b.x, b.y);
    lv_obj_set_size(s_detail, b.w, b.h);
}

static void ov_ready(lv_anim_t *a)
{
    (void)a;
    if (s_ov_opening != 0) {
        return;
    }
    lv_obj_add_flag(s_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    s_overlay_open = false;
    s_ov_opening = -1;
}

static void ov_run(box_t from, box_t to, int mask_to)
{
    s_ov_from = from;
    s_ov_to = to;
    lv_anim_del(&s_ov_token, ov_exec);
    lv_anim_del(s_mask, bg_opa_exec);
    int mask_from = lv_obj_get_style_bg_opa(s_mask, 0);
    anim_i32(&s_ov_token, ov_exec, 0, 1000, UI_OVERLAY_MS, lv_anim_path_ease_out, 0, ov_ready);
    anim_i32(s_mask, bg_opa_exec, mask_from, mask_to, UI_OVERLAY_MS, lv_anim_path_ease_out, 0, NULL);
}

static void overlay_open(tile_t *t)
{
    if (t == NULL || s_overlay_open) {
        return;
    }
    s_ov_kind = t->kind;
    s_overlay_open = true;
    s_ov_opening = 1;
    lv_label_set_text_static(s_ov_title, k_tile_name[t->kind]);
    lv_label_set_text_static(s_ov_unit, k_tile_unit[t->kind]);
    lv_obj_clear_flag(s_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_mask);
    lv_obj_move_foreground(s_detail);
    overlay_refresh();
    ov_run(box_screen(t->panel), k_ov_full, UI_MASK_OPA);
}

static void overlay_close(void)
{
    if (!s_overlay_open || s_ov_opening == 0) {
        return;
    }
    s_ov_opening = 0;
    ov_run(box_of(s_detail), box_screen(s_tiles[s_ov_kind].panel), 0);
}

static void on_overlay_clicked(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    overlay_close();
}

/* ---- idle ---- */

static void idle_gone(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_idle_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_local_style_prop(s_idle_layer, LV_STYLE_OPA, 0);
    s_idle = false;
    s_waking = false;
    if (s_page != 0) {
        s_page = 0;
        s_page_gen++;
        s_page_busy = false;
        lv_anim_del(s_track, x_exec);
        lv_obj_set_x(s_track, 0);
        roll_set(&s_title, k_page_title[0], false);
        dots_layout(0, false);
    }
    start_resweep();
}

static void on_idle_press(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
        return;
    }
    if (!s_idle || s_waking) {
        return;
    }
    s_waking = true;
    lv_anim_del(s_idle_layer, fade_exec);
    lv_opa_t cur = lv_obj_get_style_opa(s_idle_layer, 0);
    anim_i32(s_idle_layer, fade_exec, cur, 0, UI_IDLE_FADE_MS, lv_anim_path_ease_in, 0, idle_gone);
}

static void enter_idle(void)
{
    if (s_idle || s_phase != PHASE_LIVE || s_overlay_open) {
        return;
    }
    s_idle = true;
    s_waking = false;
    lv_obj_set_style_opa(s_idle_layer, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_idle_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_idle_layer);
    lv_anim_del(s_idle_layer, fade_exec);
    anim_i32(s_idle_layer, fade_exec, 0, LV_OPA_COVER, UI_IDLE_FADE_MS, lv_anim_path_ease_out, 0, opa_drop);
}

/* ---- system / about builders ---- */

static const char *chip_model_str(esp_chip_model_t model)
{
    if (model == CHIP_ESP32S3) {
        return "ESP32-S3";
    }
    if (model == CHIP_ESP32S2) {
        return "ESP32-S2";
    }
    if (model == CHIP_ESP32C3) {
        return "ESP32-C3";
    }
    if (model == CHIP_ESP32) {
        return "ESP32";
    }
    return "ESP";
}

static const char *reset_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "int wdt";
    case ESP_RST_TASK_WDT:
        return "task wdt";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    case ESP_RST_EFUSE:
        return "efuse";
    case ESP_RST_PWR_GLITCH:
        return "power glitch";
    case ESP_RST_CPU_LOCKUP:
        return "cpu lockup";
    case ESP_RST_UNKNOWN:
    default:
        return "unknown";
    }
}

static void sys_set(sys_id_t id, const char *text)
{
    sys_row_t *r = &s_sys[id];
    snprintf(r->buf, sizeof r->buf, "%s", text);
    lv_label_set_text_static(r->val, r->buf);
}

static void fill_sys(void)
{
    esp_chip_info_t info;
    memset(&info, 0, sizeof info);
    esp_chip_info(&info);
    unsigned maj = (unsigned)(info.revision / 100);
    unsigned minr = (unsigned)(info.revision % 100);
    char line[64];
    snprintf(line, sizeof line, "%s rev %u.%u", chip_model_str(info.model), maj, minr);
    sys_set(SYS_CHIP, line);
    snprintf(line, sizeof line, "%u", (unsigned)info.cores);
    sys_set(SYS_CORES, line);
    snprintf(line, sizeof line, "%d MHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    sys_set(SYS_CPU, line);

    uint32_t flash = 0;
    if (esp_flash_get_size(NULL, &flash) == ESP_OK && flash >= 1024u * 1024u) {
        snprintf(line, sizeof line, "%u MB", (unsigned)(flash / (1024u * 1024u)));
    } else if (esp_flash_get_size(NULL, &flash) == ESP_OK) {
        snprintf(line, sizeof line, "%u KB", (unsigned)(flash / 1024u));
    } else {
        snprintf(line, sizeof line, "--");
    }
    sys_set(SYS_FLASH, line);

    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    snprintf(line, sizeof line, "%u MB", (unsigned)(psram / (1024u * 1024u)));
    sys_set(SYS_PSRAM, line);

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(line, sizeof line, "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(line, sizeof line, "--");
    }
    sys_set(SYS_MAC, line);

    const char *idf = esp_get_idf_version();
    sys_set(SYS_IDF, idf ? idf : "--");
    snprintf(line, sizeof line, "%d.%d.%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    sys_set(SYS_LVGL, line);
    sys_set(SYS_RESET, reset_str(esp_reset_reason()));
    lv_label_set_text_static(s_sys[SYS_UPTIME].val, s_uptime);
    sys_set(SYS_RTC, "--");
    sys_set(SYS_PERIPH, "I2C ok  " BULLET "  CAN off  " BULLET "  RS485 115200");
    refresh_minheap();
}

static void add_sys_row(int idx, bool bar)
{
    sys_row_t *r = &s_sys[idx];
    r->home_x = 24;
    r->bar = NULL;
    r->row = lv_obj_create(s_page_obj[BASHCAT_PAGE_SYS]);
    make_plain(r->row);
    lv_obj_clear_flag(r->row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(r->row, r->home_x, (lv_coord_t)(4 + idx * UI_SYS_PITCH));
    lv_obj_set_size(r->row, 752, UI_SYS_PITCH);
    lv_obj_set_style_bg_opa(r->row, LV_OPA_TRANSP, 0);

    lv_obj_t *key = make_label(r->row, &lv_font_montserrat_12, HEX_MUTED);
    lv_label_set_text_static(key, k_sys_key[idx]);
    lv_obj_set_pos(key, 8, 2);

    r->val = make_label(r->row, &lv_font_montserrat_16, HEX_TEXT);
    lv_obj_set_width(r->val, 520);
    lv_obj_set_style_text_align(r->val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(r->val, LV_ALIGN_TOP_RIGHT, -8, 0);
    r->buf[0] = '\0';
    lv_label_set_text_static(r->val, r->buf);

    lv_obj_t *rule = lv_obj_create(r->row);
    make_plain(rule);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(rule, 736, 1);
    lv_obj_align(rule, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(rule, hex(HEX_LINE), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);

    if (!bar) {
        return;
    }
    lv_obj_t *track = lv_obj_create(r->row);
    make_plain(track);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(track, 8, 21);
    lv_obj_set_size(track, 720, 3);
    lv_obj_set_style_bg_color(track, hex(HEX_LINE), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 2, 0);
    r->bar = lv_obj_create(track);
    make_plain(r->bar);
    lv_obj_clear_flag(r->bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r->bar, 0, 3);
    lv_obj_set_style_bg_color(r->bar, hex(HEX_ACCENT), 0);
    lv_obj_set_style_bg_opa(r->bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r->bar, 2, 0);
}

static lv_obj_t *about_line(int idx, const lv_font_t *font, uint32_t color, const char *text,
                            lv_coord_t y, lv_coord_t spacing)
{
    lv_obj_t *lbl = make_label(s_page_obj[BASHCAT_PAGE_ABOUT], font, color);
    if (spacing) {
        lv_obj_set_style_text_letter_space(lbl, spacing, 0);
    }
    lv_obj_set_width(lbl, 760);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_static(lbl, text);
    lv_obj_set_pos(lbl, 20, y);
    s_about_lbl[idx] = lbl;
    s_about_y[idx] = y;
    return lbl;
}

static void build_about(void)
{
    about_line(0, &lv_font_montserrat_48, HEX_TEXT, "BASHCAT", 16, 6);
    about_line(1, &lv_font_montserrat_16, HEX_DIM, "Turning ideas into things that move.", 78, 0);

    s_qr_card = lv_obj_create(s_page_obj[BASHCAT_PAGE_ABOUT]);
    make_plain(s_qr_card);
    lv_obj_clear_flag(s_qr_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_qr_card, UI_QR_CARD, UI_QR_CARD);
    lv_obj_set_pos(s_qr_card, (UI_W - UI_QR_CARD) / 2, 112);
    lv_obj_set_style_bg_color(s_qr_card, hex(HEX_TEXT), 0);
    lv_obj_set_style_bg_opa(s_qr_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_qr_card, 12, 0);
    lv_obj_set_style_border_width(s_qr_card, 0, 0);
    lv_obj_set_style_shadow_width(s_qr_card, 0, 0);
    lv_obj_set_style_transform_pivot_x(s_qr_card, UI_QR_CARD / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_qr_card, UI_QR_CARD / 2, 0);
    lv_obj_set_style_transform_zoom(s_qr_card, LV_IMG_ZOOM_NONE, 0);

    lv_obj_t *qr = lv_qrcode_create(s_qr_card, UI_QR_SIZE, hex(HEX_BG), hex(HEX_TEXT));
    lv_obj_clear_flag(qr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(qr, UI_QR_QUIET, UI_QR_QUIET);
    const char *url = "https://bashcat.net";
    lv_qrcode_update(qr, url, (uint32_t)strlen(url));

    about_line(2, &lv_font_montserrat_16, HEX_TEXT, "bashcat.net", 318, 0);
    about_line(3, &lv_font_montserrat_12, HEX_MUTED, "github.com/Oliver0804/bashcat-dash", 346, 0);
}

static void build_overlay(void)
{
    s_mask = lv_obj_create(s_dash);
    make_plain(s_mask);
    lv_obj_set_pos(s_mask, 0, 0);
    lv_obj_set_size(s_mask, UI_W, UI_H);
    lv_obj_set_style_bg_color(s_mask, hex(HEX_BG), 0);
    lv_obj_set_style_bg_opa(s_mask, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_mask, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_mask, on_overlay_clicked, LV_EVENT_CLICKED, NULL);

    s_detail = make_panel(s_dash);
    lv_obj_set_pos(s_detail, k_ov_full.x, k_ov_full.y);
    lv_obj_set_size(s_detail, k_ov_full.w, k_ov_full.h);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_detail, on_overlay_clicked, LV_EVENT_CLICKED, NULL);

    s_ov_title = make_label(s_detail, &lv_font_montserrat_12, HEX_MUTED);
    lv_label_set_text_static(s_ov_title, k_tile_name[TILE_HEAP]);
    lv_obj_set_pos(s_ov_title, 20, 16);

    lv_obj_t *closer = make_label(s_detail, &lv_font_montserrat_20, HEX_DIM);
    lv_label_set_text_static(closer, "X");
    lv_obj_align(closer, LV_ALIGN_TOP_RIGHT, -16, 12);

    s_ov_value = make_label(s_detail, &lv_font_montserrat_48, HEX_TEXT);
    lv_obj_set_pos(s_ov_value, 20, 36);
    snprintf(s_ov_value_buf, sizeof s_ov_value_buf, "0");
    lv_label_set_text_static(s_ov_value, s_ov_value_buf);

    s_ov_unit = make_label(s_detail, &lv_font_montserrat_16, HEX_MUTED);
    lv_label_set_text_static(s_ov_unit, k_tile_unit[TILE_HEAP]);
    lv_obj_set_pos(s_ov_unit, 210, 62);

    s_ov_stats = make_label(s_detail, &lv_font_montserrat_12, HEX_DIM);
    lv_obj_set_width(s_ov_stats, 480);
    lv_label_set_long_mode(s_ov_stats, LV_LABEL_LONG_CLIP);
    snprintf(s_ov_stats_buf, sizeof s_ov_stats_buf, "min --    max --    avg --");
    lv_label_set_text_static(s_ov_stats, s_ov_stats_buf);
    lv_obj_set_pos(s_ov_stats, 20, 96);

    s_ov_chart = lv_chart_create(s_detail);
    make_plain(s_ov_chart);
    lv_obj_clear_flag(s_ov_chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_ov_chart, 16, 122);
    lv_obj_set_size(s_ov_chart, 488, 160);
    lv_obj_set_style_bg_opa(s_ov_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ov_chart, 0, 0);
    lv_obj_set_style_pad_all(s_ov_chart, 4, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_ov_chart, hex(HEX_LINE), LV_PART_MAIN);
    lv_obj_set_style_line_width(s_ov_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(s_ov_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(s_ov_chart, 0, LV_PART_INDICATOR);
    lv_chart_set_type(s_ov_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_ov_chart, UI_CHART_POINTS);
    lv_chart_set_range(s_ov_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(s_ov_chart, 3, 0);
    s_ov_series = lv_chart_add_series(s_ov_chart, hex(HEX_ACCENT), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(s_ov_chart, s_ov_series, LV_CHART_POINT_NONE);
}

static void build_idle(void)
{
    s_idle_layer = lv_obj_create(s_dash);
    make_plain(s_idle_layer);
    lv_obj_set_pos(s_idle_layer, 0, 0);
    lv_obj_set_size(s_idle_layer, UI_W, UI_H);
    lv_obj_set_style_bg_color(s_idle_layer, hex(HEX_BG), 0);
    lv_obj_set_style_bg_opa(s_idle_layer, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_idle_layer, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_idle_layer, on_idle_press, LV_EVENT_PRESSED, NULL);

    s_idle_clock = make_label(s_idle_layer, &lv_font_montserrat_48, HEX_TEXT);
    lv_label_set_text_static(s_idle_clock, s_uptime);
    lv_obj_align(s_idle_clock, LV_ALIGN_CENTER, 0, -12);

    s_idle_line = lv_obj_create(s_idle_layer);
    make_plain(s_idle_line);
    lv_obj_clear_flag(s_idle_line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_idle_line, 220, 2);
    lv_obj_set_style_bg_color(s_idle_line, hex(HEX_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_idle_line, LV_OPA_COVER, 0);
    lv_obj_align(s_idle_line, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t *site = make_label(s_idle_layer, &lv_font_montserrat_12, HEX_MUTED);
    lv_label_set_text_static(site, "bashcat.net");
    lv_obj_align(site, LV_ALIGN_BOTTOM_MID, 0, -28);
    start_idle_line();
}

static lv_obj_t *make_page(int index)
{
    lv_obj_t *page = lv_obj_create(s_track);
    make_plain(page);
    lv_obj_set_pos(page, index * UI_W, 0);
    lv_obj_set_size(page, UI_W, UI_H - UI_HEADER_H);
    lv_obj_set_style_bg_color(page, hex(HEX_BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    s_page_obj[index] = page;
    return page;
}

static void build_dashboard(void)
{
    init_press_style();

    s_dash = lv_obj_create(NULL);
    style_screen(s_dash, HEX_BG);

    lv_obj_t *brand = make_label(s_dash, &lv_font_montserrat_20, HEX_TEXT);
    lv_obj_set_style_text_letter_space(brand, 6, 0);
    lv_label_set_text_static(brand, "BASHCAT");
    lv_obj_set_pos(brand, 18, 8);

    s_status_dot = lv_obj_create(s_dash);
    make_plain(s_status_dot);
    lv_obj_clear_flag(s_status_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_status_dot, 8, 8);
    lv_obj_set_pos(s_status_dot, 214, 16);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_status_dot, hex(HEX_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, 0);

    lv_obj_t *online = make_label(s_dash, &lv_font_montserrat_12, HEX_ACCENT);
    lv_label_set_text_static(online, "ONLINE");
    lv_obj_set_pos(online, 228, 13);

    roll_init(&s_title, s_dash, &lv_font_montserrat_12, HEX_DIM, 210, 16, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_title.clip, 18, 31);
    roll_set(&s_title, k_page_title[0], false);

    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        s_dot[i] = lv_obj_create(s_dash);
        make_plain(s_dot[i]);
        lv_obj_set_size(s_dot[i], UI_DOT_OFF_W, UI_DOT_H);
        lv_obj_set_y(s_dot[i], 18);
        lv_obj_set_style_radius(s_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_dot[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_dot[i], hex(HEX_MUTED), 0);
        lv_obj_set_ext_click_area(s_dot[i], 2);
        lv_obj_add_event_cb(s_dot[i], on_dot_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    dots_layout(0, false);

    s_uptime_lbl = make_label(s_dash, &lv_font_montserrat_16, HEX_TEXT);
    snprintf(s_uptime, sizeof s_uptime, "T+00:00:00");
    lv_label_set_text_static(s_uptime_lbl, s_uptime);
    lv_obj_align(s_uptime_lbl, LV_ALIGN_TOP_RIGHT, -16, 10);

    lv_obj_t *geo = make_label(s_dash, &lv_font_montserrat_12, HEX_MUTED);
    lv_label_set_text_static(geo, "25.033\xC2\xB0" "N  " BULLET "  121.565\xC2\xB0" "E  " BULLET "  est. 2018");
    lv_obj_align(geo, LV_ALIGN_TOP_RIGHT, -16, 30);

    lv_obj_t *rule = lv_obj_create(s_dash);
    make_plain(rule);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(rule, UI_W - 32, 1);
    lv_obj_set_pos(rule, 16, 48);
    lv_obj_set_style_bg_color(rule, hex(HEX_LINE), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);

    s_viewport = lv_obj_create(s_dash);
    make_plain(s_viewport);
    lv_obj_clear_flag(s_viewport, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_viewport, 0, UI_HEADER_H);
    lv_obj_set_size(s_viewport, UI_W, UI_H - UI_HEADER_H);
    lv_obj_set_style_bg_color(s_viewport, hex(HEX_BG), 0);
    lv_obj_set_style_bg_opa(s_viewport, LV_OPA_COVER, 0);

    s_track = lv_obj_create(s_viewport);
    make_plain(s_track);
    lv_obj_clear_flag(s_track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_track, 0, 0);
    lv_obj_set_size(s_track, UI_W * UI_PAGE_COUNT, UI_H - UI_HEADER_H);
    lv_obj_set_style_bg_opa(s_track, LV_OPA_TRANSP, 0);

    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        make_page(i);
    }
    lv_obj_t *dash = s_page_obj[BASHCAT_PAGE_DASH];
    bashcat_ui_v3_build(s_page_obj[BASHCAT_PAGE_IO], s_page_obj[BASHCAT_PAGE_I2C],
                        s_page_obj[BASHCAT_PAGE_CAN], s_page_obj[BASHCAT_PAGE_RS485]);

    build_gauge(&s_main, dash, 250, 4, 300, 51, 5, 8, 14, true);
    build_gauge(&s_left, dash, 16, 72, 164, 21, 4, 5, 10, false);
    build_gauge(&s_right, dash, 620, 72, 164, 21, 4, 5, 10, false);
    s_main.metric = METRIC_CPU;
    s_left.metric = METRIC_SRAM;
    s_right.metric = METRIC_PSRAM;
    s_left.slot = "02";
    s_right.slot = "03";
    s_main.move = &s_move[0];
    s_left.move = &s_move[1];
    s_right.move = &s_move[2];
    s_move[0].g = &s_main;
    s_move[1].g = &s_left;
    s_move[2].g = &s_right;
    gauge_texts(&s_main, false);
    gauge_texts(&s_left, false);
    gauge_texts(&s_right, false);
    lv_obj_add_event_cb(s_main.obj, on_gauge_clicked, LV_EVENT_CLICKED, &s_main);
    lv_obj_add_event_cb(s_left.obj, on_gauge_clicked, LV_EVENT_CLICKED, &s_left);
    lv_obj_add_event_cb(s_right.obj, on_gauge_clicked, LV_EVENT_CLICKED, &s_right);

    lv_obj_t *chart_panel = make_panel(dash);
    lv_obj_set_pos(chart_panel, 16, 312);
    lv_obj_set_size(chart_panel, 336, 104);
    lv_obj_set_style_opa(chart_panel, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(chart_panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *chart_title = make_label(chart_panel, &lv_font_montserrat_12, HEX_MUTED);
    lv_label_set_text_static(chart_title, "04  " BULLET "  load");
    lv_obj_set_pos(chart_title, 12, 8);

    s_chart = lv_chart_create(chart_panel);
    make_plain(s_chart);
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_chart, 8, 26);
    lv_obj_set_size(s_chart, 316, 70);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_pad_all(s_chart, 4, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_chart, hex(HEX_LINE), LV_PART_MAIN);
    lv_obj_set_style_line_width(s_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(s_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(s_chart, 0, LV_PART_INDICATOR);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(s_chart, UI_CHART_POINTS);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(s_chart, 3, 0);
    s_series = lv_chart_add_series(s_chart, hex(HEX_ACCENT), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(s_chart, s_series, LV_CHART_POINT_NONE);

    build_tile(&s_tiles[TILE_HEAP], dash, 360, 312, TILE_HEAP, "HEAP", "KB");
    build_tile(&s_tiles[TILE_PSRAM], dash, 468, 312, TILE_PSRAM, "PSRAM", "MB");
    build_tile(&s_tiles[TILE_TEMP], dash, 576, 312, TILE_TEMP, "CHIP TEMP", "\xC2\xB0" "C");
    build_tile(&s_tiles[TILE_FPS], dash, 684, 312, TILE_FPS, "FPS", "fps");

    for (int i = 0; i < SYS_COUNT; i++) {
        add_sys_row(i, i == SYS_MINHEAP);
    }
    fill_sys();
    build_about();
    build_overlay();
    build_idle();

    lv_obj_add_event_cb(s_dash, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_clear_flag(s_dash, LV_OBJ_FLAG_SCROLLABLE);
}

/* ---- intro ---- */

static void intro_in_exec(void *var, int32_t v)
{
    lv_obj_t *lbl = (lv_obj_t *)var;
    lv_obj_set_style_text_opa(lbl, (lv_opa_t)((v * 255) / 1000), 0);
    lv_obj_set_y(lbl, s_wordmark_y + 28 - (28 * (lv_coord_t)v) / 1000);
}

static void intro_line_exec(void *var, int32_t w)
{
    lv_obj_t *line = (lv_obj_t *)var;
    lv_obj_set_width(line, (lv_coord_t)w);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, 36);
}

static void intro_out_exec(void *var, int32_t v)
{
    lv_obj_t *lbl = (lv_obj_t *)var;
    lv_opa_t opa = (lv_opa_t)(((int)v * 255) / 1000);
    lv_obj_set_style_text_opa(lbl, opa, 0);
    if (s_intro_line) {
        lv_obj_set_style_bg_opa(s_intro_line, opa, 0);
    }
    lv_obj_set_y(lbl, s_wordmark_y - ((1000 - (lv_coord_t)v) * 26) / 1000);
}

static const char *can_word(io_can_state_t st)
{
    switch (st) {
    case IO_CAN_RUNNING:
        return "running";
    case IO_CAN_ERR_PASSIVE:
        return "passive";
    case IO_CAN_BUS_OFF:
        return "bus off";
    case IO_CAN_RECOVERING:
        return "recovering";
    default:
        return "off";
    }
}

static void sys_set_if(sys_id_t id, const char *text)
{
    if (strcmp(s_sys[id].buf, text) == 0) {
        return;
    }
    sys_set(id, text);
}

static void io_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    static io_state_t st;
    char line[64];
    bashcat_io_snapshot(&st);
    bashcat_ui_v3_apply(&st);
    sys_set_if(SYS_RTC, st.rtc_ok ? st.rtc_hms : "--");
    snprintf(line, sizeof line, "I2C %s  " BULLET "  CAN %s  " BULLET "  RS485 %u",
             st.scan_busy ? "scan" : "ok", can_word(st.can_state), (unsigned)st.rs485_baud);
    sys_set_if(SYS_PERIPH, line);
}

static void dash_reveal(lv_anim_t *a)
{
    (void)a;
    lv_scr_load(s_dash);
    if (s_intro) {
        lv_obj_del(s_intro);
        s_intro = NULL;
        s_wordmark = NULL;
        s_intro_line = NULL;
    }
    cpu_reprime();
    sample_cpu();
    fps_reset();
    sample_mem();
    sample_temp();
    update_uptime();
    start_breath();
    stagger_fade(lv_obj_get_parent(s_chart), 0);
    for (int i = 0; i < TILE_COUNT; i++) {
        stagger_fade(s_tiles[i].panel, (uint32_t)(i + 1));
        anim_tile(&s_tiles[i], tile_target(s_tiles[i].kind));
    }
    s_timer = lv_timer_create(sample_timer_cb, UI_SAMPLE_MS, NULL);
    s_io_timer = lv_timer_create(io_timer_cb, UI_IO_POLL_MS, NULL);
    start_sweep();
}

static void intro_line_ready(lv_anim_t *a)
{
    (void)a;
    anim_i32(s_wordmark, intro_out_exec, 1000, 0, UI_INTRO_OUT_MS, lv_anim_path_ease_in, UI_INTRO_GAP2_MS,
             dash_reveal);
}

static void intro_in_ready(lv_anim_t *a)
{
    (void)a;
    anim_i32(s_intro_line, intro_line_exec, 0, UI_LINE_W, UI_INTRO_LINE_MS, lv_anim_path_ease_in_out,
             UI_INTRO_GAP1_MS, intro_line_ready);
}

static void start_intro(void)
{
    s_intro = lv_scr_act();
    lv_obj_clean(s_intro);
    style_screen(s_intro, 0x000000);

    s_wordmark = make_label(s_intro, &lv_font_montserrat_24, HEX_TEXT);
    lv_obj_set_style_text_letter_space(s_wordmark, 8, 0);
    lv_label_set_text_static(s_wordmark, "BASHCAT");
    lv_obj_set_style_text_opa(s_wordmark, LV_OPA_TRANSP, 0);
    lv_obj_align(s_wordmark, LV_ALIGN_CENTER, 0, -8);
    s_wordmark_y = lv_obj_get_y(s_wordmark);

    s_intro_line = lv_obj_create(s_intro);
    make_plain(s_intro_line);
    lv_obj_set_style_bg_color(s_intro_line, hex(HEX_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_intro_line, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_intro_line, 0, 0);
    lv_obj_set_size(s_intro_line, 0, 2);
    lv_obj_align(s_intro_line, LV_ALIGN_CENTER, 0, 36);

    anim_i32(s_wordmark, intro_in_exec, 0, 1000, UI_INTRO_IN_MS, lv_anim_path_ease_out, 0, intro_in_ready);
}

void bashcat_ui_start(void)
{
    sensors_init();
    install_fps_monitor();
    build_dashboard();
    start_intro();
    cpu_reprime();
    sample_cpu();
}
