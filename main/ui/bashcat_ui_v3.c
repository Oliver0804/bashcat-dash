/*
 * v3 pages: IO, I2C, CAN, RS485. Built once into the page track.
 * Widgets follow bashcat_io_snapshot(); they never call a driver.
 */

#include "bashcat_ui_v3.h"

#include <stdio.h>
#include <string.h>

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

#define UI_PRESS_MS        120
#define UI_PULSE_MS        400
#define UI_SCAN_STAGGER_MS 30
#define UI_LAMP_MS         80
#define UI_POP_MS          220
#define UI_SEND_MS         400
#define UI_NUM_MS          300
#define UI_SEG_MS          320
#define UI_ROW_MS          240
#define UI_ENTER_MS        280
#define UI_STAGGER_MS      40
#define UI_SLIDE_PX        16
#define UI_ROLL_MS         280
#define UI_DI_POINTS       100
#define UI_CAN_ROWS        8
#define UI_RS_ROWS         8
#define UI_LIST_MAX        8
#define CELL_W             22
#define CELL_H             20
#define CAN_ROW_H          22
#define RS_ROW_H           28
#define BULLET             "\xE2\x80\xA2"

#define BLK_MAX 6

typedef struct {
    lv_obj_t *clip;
    lv_obj_t *lab[2];
    char buf[2][32];
    int vis;
    lv_coord_t h;
} roll_t;

typedef struct {
    lv_obj_t *obj;
    lv_coord_t home_x;
} blk_t;

typedef struct {
    lv_obj_t *ring;
    lv_coord_t cx;
    lv_coord_t cy;
    lv_coord_t base;
} pulse_t;

typedef struct {
    lv_obj_t *track;
    lv_obj_t *pill;
    lv_obj_t *lbl[5];
    int n;
    int sel;
    lv_coord_t seg_w;
} seg_t;

typedef struct {
    lv_obj_t *lbl;
    char buf[24];
    int shown;
    int target;
} num_t;

typedef struct {
    lv_obj_t *lbl;
    char buf[20];
    char pending[20];
    int phase;
} fade_t;

static const lv_style_prop_t k_press_props[] = {
    LV_STYLE_BG_COLOR, LV_STYLE_BG_OPA, LV_STYLE_BORDER_COLOR, 0,
};
static const lv_style_prop_t k_color_props[] = {
    LV_STYLE_BG_COLOR, LV_STYLE_TEXT_COLOR, 0,
};
static lv_style_transition_dsc_t s_press_tr;
static lv_style_transition_dsc_t s_color_tr;

static blk_t s_blk[4][BLK_MAX];
static int s_blk_n[4];

static pulse_t s_pulse[2];
static lv_obj_t *s_lamp[2];
static roll_t s_changes[2];
static lv_obj_t *s_sw[2];
static roll_t s_do_roll[2];
static int s_do_want[2] = {-1, -1};
static bool s_do_sync;
static uint8_t s_prev_di;
static bool s_have_di;
static lv_obj_t *s_di_chart[2];
static lv_chart_series_t *s_di_ser[2];

static lv_obj_t *s_cell[128];
static char s_cell_txt[128][3];
static uint8_t s_popped[16];
static int s_pop_n;
static lv_obj_t *s_cursor;
static int s_hi_addr = -1;
static bool s_scan_busy_prev;
static bool s_ch422_listed;
static lv_obj_t *s_scan_btn;
static lv_obj_t *s_list_row[UI_LIST_MAX];
static lv_obj_t *s_list_lbl[UI_LIST_MAX];
static char s_list_buf[UI_LIST_MAX][40];
static int s_list_n;
static roll_t s_rtc_roll;
static lv_obj_t *s_rtc_date;
static char s_rtc_date_buf[20];

static seg_t s_can_seg;
static seg_t s_rs_seg;
static lv_obj_t *s_listen;
static bool s_listen_sync;
static lv_obj_t *s_start_btn;
static fade_t s_start_fade;
static lv_obj_t *s_send_btn;
static lv_obj_t *s_send_bar;
static lv_obj_t *s_can_lamp;
static fade_t s_can_fade;
static int s_can_shown = -1;
static num_t s_can_num[5];
static lv_obj_t *s_can_row[UI_CAN_ROWS];
static lv_obj_t *s_can_lbl[UI_CAN_ROWS];
static char s_can_txt[UI_CAN_ROWS][72];
static lv_obj_t *s_can_ghost;
static char s_can_ghost_buf[72];
static uint32_t s_can_seq_seen;
static bool s_can_seq_init;
static lv_obj_t *s_spark;
static lv_chart_series_t *s_spark_ser;
static uint32_t s_spark_ms;
static uint32_t s_spark_rx;
static bool s_spark_init;
static uint32_t s_payload;
static bool s_can_run;
static int s_bitrate_idx;

static lv_obj_t *s_rx_lamp;
static lv_obj_t *s_tx_lamp;
static num_t s_rs_num[2];
static lv_obj_t *s_rs_row[UI_RS_ROWS];
static lv_obj_t *s_rs_mark[UI_RS_ROWS];
static lv_obj_t *s_rs_lbl[UI_RS_ROWS];
static char s_rs_txt[UI_RS_ROWS][IO_LINE_HEX];
static lv_obj_t *s_rs_ghost;
static char s_rs_ghost_buf[IO_LINE_HEX];
static uint32_t s_log_seen;
static bool s_log_init;
static uint32_t s_rx_seq_seen;
static uint32_t s_tx_seq_seen;
static bool s_rs_seq_init;
static uint32_t s_ping_n;

static const uint32_t k_bitrate[] = {50000, 125000, 250000, 500000, 1000000};
static const char *const k_bitrate_lbl[] = {"50k", "125k", "250k", "500k", "1M"};
static const uint32_t k_baud[] = {9600, 19200, 57600, 115200};
static const char *const k_baud_lbl[] = {"9600", "19200", "57600", "115200"};

static lv_color_t hex(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static void anim_i32(void *var, lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t time_ms,
                     lv_anim_path_cb_t path, uint32_t delay_ms, lv_anim_ready_cb_t ready, void *ud)
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
    lv_anim_set_user_data(&a, ud);
    lv_anim_start(&a);
}

static void make_plain(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, hex(color), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

static lv_obj_t *make_panel(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    make_plain(p);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(p, hex(HEX_PANEL), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_border_color(p, hex(HEX_LINE), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    return p;
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
static void fade_exec(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void noop_exec(void *var, int32_t v)
{
    (void)var;
    (void)v;
}

/* Cover entrances must not leave a local opa style behind. Fade-outs
 * that end transparent are hidden first, or removing the prop would
 * make them opaque again. */
static void opa_drop(lv_anim_t *a)
{
    lv_obj_t *obj = a->var;
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    lv_obj_remove_local_style_prop(obj, LV_STYLE_OPA, 0);
}

static void opa_hide(lv_anim_t *a)
{
    lv_obj_t *obj = a->var;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_local_style_prop(obj, LV_STYLE_OPA, 0);
}
static void text_opa_exec(void *var, int32_t v)
{
    lv_obj_set_style_text_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}
static void bg_opa_exec(void *var, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}
static void zoom_exec(void *var, int32_t v)
{
    lv_obj_set_style_transform_zoom((lv_obj_t *)var, (lv_coord_t)v, 0);
}

static void roll_init(roll_t *r, lv_obj_t *parent, const lv_font_t *font, uint32_t color, lv_coord_t w,
                      lv_coord_t h, lv_text_align_t align)
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
    if (r == NULL || text == NULL || r->lab[0] == NULL) {
        return;
    }
    if (strcmp(r->buf[r->vis], text) == 0) {
        return;
    }
    if (!animate) {
        snprintf(r->buf[0], sizeof r->buf[0], "%s", text);
        lv_label_set_text_static(r->lab[0], r->buf[0]);
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
    snprintf(r->buf[neu], sizeof r->buf[neu], "%s", text);
    lv_label_set_text_static(r->lab[neu], r->buf[neu]);
    lv_anim_del(r->lab[old], y_exec);
    lv_anim_del(r->lab[old], text_opa_exec);
    lv_anim_del(r->lab[neu], y_exec);
    lv_anim_del(r->lab[neu], text_opa_exec);
    lv_obj_set_y(r->lab[old], 0);
    lv_obj_set_style_text_opa(r->lab[old], LV_OPA_COVER, 0);
    lv_obj_set_y(r->lab[neu], r->h);
    lv_obj_set_style_text_opa(r->lab[neu], LV_OPA_TRANSP, 0);
    anim_i32(r->lab[old], y_exec, 0, -(int32_t)r->h, UI_ROLL_MS, lv_anim_path_ease_out, 0, NULL, NULL);
    anim_i32(r->lab[old], text_opa_exec, LV_OPA_COVER, 0, UI_ROLL_MS, lv_anim_path_ease_out, 0, NULL, NULL);
    anim_i32(r->lab[neu], y_exec, r->h, 0, UI_ROLL_MS, lv_anim_path_ease_out, 0, NULL, NULL);
    anim_i32(r->lab[neu], text_opa_exec, 0, LV_OPA_COVER, UI_ROLL_MS, lv_anim_path_ease_out, 0, NULL, NULL);
    r->vis = neu;
}

static void style_btn(lv_obj_t *btn)
{
    make_plain(btn);
    lv_obj_set_style_bg_color(btn, hex(HEX_PANEL), 0);
    lv_obj_set_style_bg_color(btn, hex(HEX_PRESS), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, hex(HEX_LINE), 0);
    lv_obj_set_style_border_color(btn, hex(HEX_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_opa(btn, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_style_transition(btn, &s_press_tr, 0);
    lv_obj_set_style_transition(btn, &s_press_tr, LV_STATE_PRESSED);
}

static lv_obj_t *btn_label(lv_obj_t *btn, const char *text)
{
    lv_obj_t *lbl = make_label(btn, &lv_font_montserrat_16, HEX_TEXT);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return lbl;
}

static void blk_add(int page, lv_obj_t *obj)
{
    int i = page - BASHCAT_PAGE_IO;
    if (i < 0 || i > 3 || s_blk_n[i] >= BLK_MAX) {
        return;
    }
    s_blk[i][s_blk_n[i]].obj = obj;
    /* Pages sit off the viewport during build, so coords are still 0 and
     * lv_obj_get_x() would pin every block to the left edge. Style x is
     * the value lv_obj_set_pos() stored. */
    s_blk[i][s_blk_n[i]].home_x = lv_obj_get_style_x(obj, LV_PART_MAIN);
    s_blk_n[i]++;
}

static void blk_prime(int i)
{
    for (int n = 0; n < s_blk_n[i]; n++) {
        lv_obj_t *o = s_blk[i][n].obj;
        lv_anim_del(o, x_exec);
        lv_anim_del(o, fade_exec);
        lv_obj_set_x(o, (lv_coord_t)(s_blk[i][n].home_x + UI_SLIDE_PX));
        lv_obj_set_style_opa(o, LV_OPA_TRANSP, 0);
    }
}

static void blk_play(int i)
{
    for (int n = 0; n < s_blk_n[i]; n++) {
        lv_coord_t home = s_blk[i][n].home_x;
        uint32_t delay = (uint32_t)n * UI_STAGGER_MS;
        anim_i32(s_blk[i][n].obj, x_exec, home + UI_SLIDE_PX, home, UI_ENTER_MS, lv_anim_path_ease_out,
                 delay, NULL, NULL);
        anim_i32(s_blk[i][n].obj, fade_exec, 0, LV_OPA_COVER, UI_ENTER_MS, lv_anim_path_ease_out, delay,
                 opa_drop, NULL);
    }
}

static void pulse_exec(void *var, int32_t v)
{
    pulse_t *p = (pulse_t *)var;
    int size = (int)p->base + (int)((48 * v) / 1000);
    lv_obj_clear_flag(p->ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(p->ring, (lv_coord_t)size, (lv_coord_t)size);
    lv_obj_set_pos(p->ring, (lv_coord_t)(p->cx - size / 2), (lv_coord_t)(p->cy - size / 2));
    lv_obj_set_style_border_opa(p->ring, (lv_opa_t)(255 - (255 * v) / 1000), 0);
}

static void lamp_set(lv_obj_t *lamp, bool on)
{
    if (on) {
        lv_obj_set_style_bg_color(lamp, hex(HEX_ACCENT), 0);
        lv_obj_set_style_bg_opa(lamp, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(lamp, hex(HEX_ACCENT), 0);
    } else {
        lv_obj_set_style_bg_opa(lamp, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(lamp, hex(HEX_LINE), 0);
    }
}

static void num_exec(void *var, int32_t v)
{
    num_t *n = (num_t *)var;
    n->shown = (int)v;
    snprintf(n->buf, sizeof n->buf, "%d", n->shown);
    lv_label_set_text_static(n->lbl, n->buf);
}

static void num_to(num_t *n, uint32_t value)
{
    int target = value > 1000000u ? 1000000 : (int)value;
    if (target == n->target) {
        return;
    }
    n->target = target;
    anim_i32(n, num_exec, n->shown, target, UI_NUM_MS, lv_anim_path_ease_out, 0, NULL, NULL);
}

static void fade_out_ready(lv_anim_t *a)
{
    fade_t *f = (fade_t *)a->user_data;
    snprintf(f->buf, sizeof f->buf, "%s", f->pending);
    lv_label_set_text_static(f->lbl, f->buf);
    f->phase = 0;
    anim_i32(f->lbl, text_opa_exec, 0, LV_OPA_COVER, 120, lv_anim_path_ease_out, 0, NULL, NULL);
}

static void fade_set(fade_t *f, const char *text)
{
    if (text == NULL) {
        return;
    }
    if (f->phase == 0 && strcmp(f->buf, text) == 0) {
        return;
    }
    snprintf(f->pending, sizeof f->pending, "%s", text);
    if (f->phase == 1) {
        return;
    }
    if (f->buf[0] == '\0') {
        snprintf(f->buf, sizeof f->buf, "%s", text);
        lv_label_set_text_static(f->lbl, f->buf);
        return;
    }
    f->phase = 1;
    anim_i32(f->lbl, text_opa_exec, LV_OPA_COVER, 0, 100, lv_anim_path_ease_in, 0, fade_out_ready, f);
}

static void seg_build(seg_t *s, lv_obj_t *parent, lv_coord_t x, lv_coord_t y, int n, lv_coord_t seg_w,
                      const char *const *labels)
{
    s->n = n;
    s->sel = 0;
    s->seg_w = seg_w;
    s->track = lv_obj_create(parent);
    make_plain(s->track);
    lv_obj_clear_flag(s->track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s->track, x, y);
    lv_obj_set_size(s->track, (lv_coord_t)(n * seg_w), 36);
    lv_obj_set_style_bg_color(s->track, hex(HEX_LINE), 0);
    lv_obj_set_style_bg_opa(s->track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s->track, 12, 0);
    s->pill = lv_obj_create(s->track);
    make_plain(s->pill);
    lv_obj_clear_flag(s->pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s->pill, 3, 3);
    lv_obj_set_size(s->pill, (lv_coord_t)(seg_w - 6), 30);
    lv_obj_set_style_bg_color(s->pill, hex(HEX_ACCENT), 0);
    lv_obj_set_style_bg_opa(s->pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s->pill, 10, 0);
    for (int i = 0; i < n; i++) {
        s->lbl[i] = make_label(s->track, &lv_font_montserrat_12, i == 0 ? HEX_BG : HEX_DIM);
        lv_label_set_text(s->lbl[i], labels[i]);
        lv_obj_set_width(s->lbl[i], seg_w);
        lv_obj_set_style_text_align(s->lbl[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s->lbl[i], (lv_coord_t)(i * seg_w), 10);
        lv_obj_set_style_transition(s->lbl[i], &s_color_tr, 0);
        lv_obj_t *hit = lv_obj_create(s->track);
        make_plain(hit);
        lv_obj_set_pos(hit, (lv_coord_t)(i * seg_w), 0);
        lv_obj_set_size(hit, seg_w, 36);
        lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
        /* Index 0 must not be stored as NULL. Callers subtract 1. */
        lv_obj_set_user_data(hit, (void *)(intptr_t)(i + 1));
    }
}

static void seg_select(seg_t *s, int idx, bool animate)
{
    if (idx < 0 || idx >= s->n) {
        return;
    }
    s->sel = idx;
    lv_coord_t dest = (lv_coord_t)(idx * s->seg_w + 3);
    if (!animate) {
        lv_obj_set_x(s->pill, dest);
    } else {
        anim_i32(s->pill, x_exec, lv_obj_get_x(s->pill), dest, UI_SEG_MS, lv_anim_path_ease_out, 0, NULL,
                 NULL);
    }
    for (int i = 0; i < s->n; i++) {
        lv_obj_set_style_text_color(s->lbl[i], hex(i == idx ? HEX_BG : HEX_DIM), 0);
    }
}

static void post_do(int bit, int value)
{
    io_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.kind = IO_CMD_SET_DO;
    cmd.mask = (uint8_t)(1u << bit);
    cmd.value = value ? cmd.mask : 0;
    bashcat_io_post(&cmd);
}

static void on_do(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED || s_do_sync) {
        return;
    }
    if (!bashcat_ui_page_ready(BASHCAT_PAGE_IO)) {
        return;
    }
    int bit = (int)(intptr_t)lv_event_get_user_data(e);
    int on = lv_obj_has_state(s_sw[bit], LV_STATE_CHECKED) ? 1 : 0;
    s_do_want[bit] = on;
    roll_set(&s_do_roll[bit], "...", true);
    post_do(bit, on);
}

static void on_read(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!bashcat_ui_page_ready(BASHCAT_PAGE_IO)) {
        return;
    }
    io_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.kind = IO_CMD_READ_DI;
    bashcat_io_post(&cmd);
}

static void on_scan(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!bashcat_ui_page_ready(BASHCAT_PAGE_I2C)) {
        return;
    }
    io_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.kind = IO_CMD_I2C_SCAN;
    bashcat_io_post(&cmd);
}

static void on_can_seg(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!bashcat_ui_page_ready(BASHCAT_PAGE_CAN)) {
        return;
    }
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e)) - 1;
    if (idx < 0 || idx >= s_can_seg.n) {
        return;
    }
    seg_select(&s_can_seg, idx, true);
    s_bitrate_idx = idx;
    if (s_can_run) {
        io_cmd_t cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.kind = IO_CMD_CAN_START;
        cmd.bitrate = k_bitrate[idx];
        cmd.listen_only = lv_obj_has_state(s_listen, LV_STATE_CHECKED);
        bashcat_io_post(&cmd);
    }
}

static void on_listen(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED || s_listen_sync) {
        return;
    }
    if (!bashcat_ui_page_ready(BASHCAT_PAGE_CAN)) {
        return;
    }
    if (!s_can_run) {
        return;
    }
    io_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.kind = IO_CMD_CAN_START;
    cmd.bitrate = k_bitrate[s_bitrate_idx];
    cmd.listen_only = lv_obj_has_state(s_listen, LV_STATE_CHECKED);
    bashcat_io_post(&cmd);
}

static void on_start(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!bashcat_ui_page_ready(BASHCAT_PAGE_CAN)) {
        return;
    }
    io_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    if (s_can_run) {
        cmd.kind = IO_CMD_CAN_STOP;
        fade_set(&s_start_fade, "START");
        s_can_run = false;
    } else {
        cmd.kind = IO_CMD_CAN_START;
        cmd.bitrate = k_bitrate[s_bitrate_idx];
        cmd.listen_only = lv_obj_has_state(s_listen, LV_STATE_CHECKED);
        fade_set(&s_start_fade, "STOP");
        s_can_run = true;
    }
    bashcat_io_post(&cmd);
}

static void on_send(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!bashcat_ui_page_ready(BASHCAT_PAGE_CAN)) {
        return;
    }
    io_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.kind = IO_CMD_CAN_SEND;
    cmd.id = 0x123;
    cmd.dlc = 4;
    s_payload++;
    cmd.data[0] = (uint8_t)(s_payload & 0xFFu);
    cmd.data[1] = (uint8_t)((s_payload >> 8) & 0xFFu);
    cmd.data[2] = (uint8_t)((s_payload >> 16) & 0xFFu);
    cmd.data[3] = (uint8_t)((s_payload >> 24) & 0xFFu);
    bashcat_io_post(&cmd);
    lv_obj_set_width(s_send_bar, 0);
    lv_obj_clear_flag(s_send_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_send_bar, LV_OPA_COVER, 0);
    anim_i32(s_send_bar, w_exec, 0, 140, UI_SEND_MS, lv_anim_path_ease_out, 0, NULL, NULL);
    anim_i32(s_send_bar, fade_exec, LV_OPA_COVER, 0, UI_SEND_MS, lv_anim_path_ease_in, 0, opa_hide, NULL);
}

static void on_baud(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!bashcat_ui_page_ready(BASHCAT_PAGE_RS485)) {
        return;
    }
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e)) - 1;
    if (idx < 0 || idx >= s_rs_seg.n) {
        return;
    }
    seg_select(&s_rs_seg, idx, true);
    io_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.kind = IO_CMD_RS485_CONFIG;
    cmd.baud = k_baud[idx];
    bashcat_io_post(&cmd);
}

static void send_bytes(const uint8_t *bytes, uint8_t len)
{
    io_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.kind = IO_CMD_RS485_SEND;
    cmd.len = len;
    memcpy(cmd.bytes, bytes, len);
    bashcat_io_post(&cmd);
}

static void on_ping(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !bashcat_ui_page_ready(BASHCAT_PAGE_RS485)) {
        return;
    }
    char msg[40];
    snprintf(msg, sizeof msg, "BASHCAT PING %u\r\n", (unsigned)s_ping_n);
    s_ping_n++;
    send_bytes((const uint8_t *)msg, (uint8_t)strlen(msg));
}

static void on_hello(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !bashcat_ui_page_ready(BASHCAT_PAGE_RS485)) {
        return;
    }
    const char *msg = "hello from bashcat.net\r\n";
    send_bytes((const uint8_t *)msg, (uint8_t)strlen(msg));
}

static void on_aa(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !bashcat_ui_page_ready(BASHCAT_PAGE_RS485)) {
        return;
    }
    const uint8_t raw[2] = {0x55, 0xAA};
    send_bytes(raw, 2);
}

static void wire_seg_hits(seg_t *s, lv_event_cb_t cb)
{
    uint32_t n = lv_obj_get_child_cnt(s->track);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(s->track, (int32_t)i);
        if (lv_obj_check_type(ch, &lv_obj_class) && lv_obj_has_flag(ch, LV_OBJ_FLAG_CLICKABLE) &&
            ch != s->pill) {
            /* Labels are not clickable. Hit pads are plain objs. */
            if (lv_obj_get_child_cnt(ch) == 0 && lv_obj_get_user_data(ch) != NULL) {
                lv_obj_add_event_cb(ch, cb, LV_EVENT_CLICKED, NULL);
            }
        }
    }
}

static lv_obj_t *make_chart(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                            int points, int ymin, int ymax, lv_chart_series_t **ser)
{
    lv_obj_t *c = lv_chart_create(parent);
    make_plain(c);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 2, LV_PART_MAIN);
    lv_obj_set_style_line_width(c, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(c, 0, LV_PART_INDICATOR);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(c, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(c, (uint16_t)points);
    lv_chart_set_range(c, LV_CHART_AXIS_PRIMARY_Y, ymin, ymax);
    lv_chart_set_div_line_count(c, 0, 0);
    *ser = lv_chart_add_series(c, hex(HEX_ACCENT), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(c, *ser, LV_CHART_POINT_NONE);
    return c;
}

static void build_io(lv_obj_t *page)
{
    lv_obj_t *head = lv_obj_create(page);
    make_plain(head);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(head, 16, 4);
    lv_obj_set_size(head, 768, 34);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_t *hint = make_label(head, &lv_font_montserrat_12, HEX_MUTED);
    lv_label_set_text_static(hint, "CH422G  " BULLET "  DI 0x01/0x20  " BULLET "  DO OD0/OD1  " BULLET "  active-low");
    lv_obj_set_pos(hint, 0, 8);
    lv_obj_t *read = lv_obj_create(head);
    style_btn(read);
    lv_obj_set_pos(read, 624, 0);
    lv_obj_set_size(read, 144, 34);
    btn_label(read, "READ");
    lv_obj_add_event_cb(read, on_read, LV_EVENT_CLICKED, NULL);
    blk_add(BASHCAT_PAGE_IO, head);

    lv_obj_t *in = make_panel(page);
    lv_obj_set_pos(in, 16, 44);
    lv_obj_set_size(in, 376, 210);
    lv_obj_t *it = make_label(in, &lv_font_montserrat_12, HEX_MUTED);
    lv_label_set_text_static(it, "INPUTS");
    lv_obj_set_pos(it, 16, 12);
    static const lv_coord_t lamp_x[2] = {36, 200};
    for (int i = 0; i < 2; i++) {
        s_pulse[i].cx = (lv_coord_t)(lamp_x[i] + 36);
        s_pulse[i].cy = 84;
        s_pulse[i].base = 72;
        s_pulse[i].ring = lv_obj_create(in);
        make_plain(s_pulse[i].ring);
        lv_obj_clear_flag(s_pulse[i].ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_pulse[i].ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(s_pulse[i].ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s_pulse[i].ring, hex(HEX_ACCENT), 0);
        lv_obj_set_style_border_width(s_pulse[i].ring, 2, 0);
        lv_obj_set_style_radius(s_pulse[i].ring, LV_RADIUS_CIRCLE, 0);
        s_lamp[i] = lv_obj_create(in);
        make_plain(s_lamp[i]);
        lv_obj_clear_flag(s_lamp[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(s_lamp[i], lamp_x[i], 48);
        lv_obj_set_size(s_lamp[i], 72, 72);
        lv_obj_set_style_radius(s_lamp[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_lamp[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s_lamp[i], hex(HEX_LINE), 0);
        lv_obj_set_style_border_width(s_lamp[i], 2, 0);
        lv_obj_t *name = make_label(in, &lv_font_montserrat_12, HEX_DIM);
        lv_label_set_text(name, i == 0 ? "DI0" : "DI1");
        lv_obj_set_pos(name, lamp_x[i], 28);
        roll_init(&s_changes[i], in, &lv_font_montserrat_12, HEX_DIM, 140, 16, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_pos(s_changes[i].clip, lamp_x[i], 156);
        roll_set(&s_changes[i], "changes: 0", false);
    }
    blk_add(BASHCAT_PAGE_IO, in);

    lv_obj_t *out = make_panel(page);
    lv_obj_set_pos(out, 408, 44);
    lv_obj_set_size(out, 376, 210);
    lv_obj_t *ot = make_label(out, &lv_font_montserrat_12, HEX_MUTED);
    lv_label_set_text_static(ot, "OUTPUTS");
    lv_obj_set_pos(ot, 16, 12);
    for (int i = 0; i < 2; i++) {
        lv_coord_t x = (i == 0) ? 28 : 200;
        lv_obj_t *nm = make_label(out, &lv_font_montserrat_12, HEX_DIM);
        lv_label_set_text(nm, i == 0 ? "DO0" : "DO1");
        lv_obj_set_pos(nm, x, 36);
        s_sw[i] = lv_switch_create(out);
        lv_obj_set_pos(s_sw[i], x, 56);
        lv_obj_set_size(s_sw[i], 156, 58);
        lv_obj_set_style_bg_color(s_sw[i], hex(HEX_LINE), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_sw[i], hex(HEX_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(s_sw[i], hex(HEX_TEXT), LV_PART_KNOB);
        lv_obj_set_style_anim_time(s_sw[i], 200, 0);
        lv_obj_add_event_cb(s_sw[i], on_do, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        roll_init(&s_do_roll[i], out, &lv_font_montserrat_16, HEX_TEXT, 156, 20, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_pos(s_do_roll[i].clip, x, 128);
        roll_set(&s_do_roll[i], "OFF", false);
    }
    blk_add(BASHCAT_PAGE_IO, out);

    lv_obj_t *wave = make_panel(page);
    lv_obj_set_pos(wave, 16, 264);
    lv_obj_set_size(wave, 768, 152);
    lv_obj_t *wt = make_label(wave, &lv_font_montserrat_12, HEX_MUTED);
    lv_label_set_text_static(wt, "DI  " BULLET "  10 s");
    lv_obj_set_pos(wt, 16, 8);
    s_di_chart[0] = make_chart(wave, 16, 28, 736, 52, UI_DI_POINTS, 0, 100, &s_di_ser[0]);
    s_di_chart[1] = make_chart(wave, 16, 86, 736, 52, UI_DI_POINTS, 0, 100, &s_di_ser[1]);
    blk_add(BASHCAT_PAGE_IO, wave);
}

static void cell_style_muted(lv_obj_t *cell)
{
    lv_obj_t *lbl = lv_obj_get_child(cell, 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_transform_zoom(cell, LV_IMG_ZOOM_NONE, 0);
    if (lbl) {
        lv_obj_set_style_text_color(lbl, hex(HEX_MUTED), 0);
    }
}

static void reset_i2c_visuals(void)
{
    memset(s_popped, 0, sizeof s_popped);
    s_pop_n = 0;
    s_list_n = 0;
    s_ch422_listed = false;
    s_hi_addr = -1;
    for (int i = 0; i < 128; i++) {
        if (s_cell[i]) {
            cell_style_muted(s_cell[i]);
        }
    }
    for (int i = 0; i < UI_LIST_MAX; i++) {
        lv_anim_del(s_list_row[i], fade_exec);
        lv_anim_del(s_list_row[i], x_exec);
        lv_obj_add_flag(s_list_row[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_local_style_prop(s_list_row[i], LV_STYLE_OPA, 0);
        s_list_buf[i][0] = '\0';
        lv_label_set_text_static(s_list_lbl[i], s_list_buf[i]);
    }
}

static void pop_cell(int addr)
{
    lv_obj_t *cell = s_cell[addr];
    if (cell == NULL) {
        return;
    }
    lv_obj_t *lbl = lv_obj_get_child(cell, 0);
    lv_obj_set_style_bg_color(cell, hex(HEX_ACCENT), 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    if (lbl) {
        lv_obj_set_style_text_color(lbl, hex(HEX_BG), 0);
    }
    lv_obj_set_style_transform_zoom(cell, 205, 0);
    uint32_t delay = (uint32_t)s_pop_n * UI_SCAN_STAGGER_MS;
    s_pop_n++;
    anim_i32(cell, zoom_exec, 205, LV_IMG_ZOOM_NONE, UI_POP_MS, lv_anim_path_ease_out, delay, NULL, NULL);
}

static void add_list_row(const char *text)
{
    if (s_list_n >= UI_LIST_MAX) {
        return;
    }
    int i = s_list_n++;
    snprintf(s_list_buf[i], sizeof s_list_buf[i], "%s", text);
    lv_label_set_text_static(s_list_lbl[i], s_list_buf[i]);
    lv_coord_t home = 12;
    lv_obj_clear_flag(s_list_row[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(s_list_row[i], (lv_coord_t)(home + UI_SLIDE_PX));
    lv_obj_set_style_opa(s_list_row[i], LV_OPA_TRANSP, 0);
    uint32_t delay = (uint32_t)i * UI_SCAN_STAGGER_MS;
    anim_i32(s_list_row[i], x_exec, home + UI_SLIDE_PX, home, UI_ENTER_MS, lv_anim_path_ease_out, delay,
             NULL, NULL);
    anim_i32(s_list_row[i], fade_exec, 0, LV_OPA_COVER, UI_ENTER_MS, lv_anim_path_ease_out, delay, opa_drop,
             NULL);
}

static void note_found(unsigned addr)
{
    char buf[40];
    if (addr >= 0x20 && addr <= 0x3F) {
        if (!s_ch422_listed) {
            s_ch422_listed = true;
            snprintf(buf, sizeof buf, "0x20-3F  CH422G");
            add_list_row(buf);
        }
        return;
    }
    if (addr == 0x51) {
        snprintf(buf, sizeof buf, "0x51  RTC  " BULLET "  PCF85063");
    } else if (addr == 0x14 || addr == 0x5D) {
        snprintf(buf, sizeof buf, "0x%02X  TOUCH  " BULLET "  GT911", addr);
    } else {
        snprintf(buf, sizeof buf, "0x%02X", addr);
    }
    add_list_row(buf);
}

static void build_i2c(lv_obj_t *page)
{
    s_scan_btn = lv_obj_create(page);
    style_btn(s_scan_btn);
    lv_obj_set_pos(s_scan_btn, 16, 8);
    lv_obj_set_size(s_scan_btn, 132, 36);
    btn_label(s_scan_btn, "SCAN");
    lv_obj_add_event_cb(s_scan_btn, on_scan, LV_EVENT_CLICKED, NULL);
    blk_add(BASHCAT_PAGE_I2C, s_scan_btn);

    lv_obj_t *grid = lv_obj_create(page);
    make_plain(grid);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(grid, 12, 50);
    lv_obj_set_size(grid, 400, 188);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    for (int col = 0; col < 16; col++) {
        lv_obj_t *h = make_label(grid, &lv_font_montserrat_12, HEX_MUTED);
        char tiny[2] = {(char)(col < 10 ? '0' + col : 'A' + (col - 10)), '\0'};
        lv_label_set_text(h, tiny);
        lv_obj_set_pos(h, (lv_coord_t)(28 + col * CELL_W + 6), 0);
    }
    for (int row = 0; row < 8; row++) {
        lv_obj_t *rl = make_label(grid, &lv_font_montserrat_12, HEX_MUTED);
        char rt[4];
        snprintf(rt, sizeof rt, "%02X", row * 16);
        lv_label_set_text(rl, rt);
        lv_obj_set_pos(rl, 0, (lv_coord_t)(18 + row * CELL_H));
    }
    for (int addr = 0; addr < 128; addr++) {
        int col = addr & 15;
        int row = addr >> 4;
        lv_obj_t *cell = lv_obj_create(grid);
        make_plain(cell);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(cell, (lv_coord_t)(28 + col * CELL_W), (lv_coord_t)(16 + row * CELL_H));
        lv_obj_set_size(cell, CELL_W, CELL_H);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(cell, 4, 0);
        lv_obj_set_style_transform_pivot_x(cell, CELL_W / 2, 0);
        lv_obj_set_style_transform_pivot_y(cell, CELL_H / 2, 0);
        snprintf(s_cell_txt[addr], sizeof s_cell_txt[addr], "%02X", addr);
        lv_obj_t *lbl = make_label(cell, &lv_font_montserrat_12, HEX_MUTED);
        lv_label_set_text_static(lbl, s_cell_txt[addr]);
        lv_obj_center(lbl);
        s_cell[addr] = cell;
    }
    s_cursor = lv_obj_create(grid);
    make_plain(s_cursor);
    lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_cursor, CELL_W, CELL_H);
    lv_obj_set_style_bg_opa(s_cursor, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_cursor, hex(HEX_ACCENT), 0);
    lv_obj_set_style_border_width(s_cursor, 2, 0);
    lv_obj_set_style_radius(s_cursor, 4, 0);
    blk_add(BASHCAT_PAGE_I2C, grid);

    lv_obj_t *list = make_panel(page);
    lv_obj_set_pos(list, 420, 8);
    lv_obj_set_size(list, 364, 210);
    lv_obj_t *lt = make_label(list, &lv_font_montserrat_12, HEX_MUTED);
    lv_label_set_text_static(lt, "FOUND");
    lv_obj_set_pos(lt, 12, 8);
    for (int i = 0; i < UI_LIST_MAX; i++) {
        s_list_row[i] = lv_obj_create(list);
        make_plain(s_list_row[i]);
        lv_obj_clear_flag(s_list_row[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(s_list_row[i], 12, (lv_coord_t)(28 + i * 22));
        lv_obj_set_size(s_list_row[i], 340, 20);
        lv_obj_set_style_bg_opa(s_list_row[i], LV_OPA_TRANSP, 0);
        lv_obj_add_flag(s_list_row[i], LV_OBJ_FLAG_HIDDEN);
        s_list_lbl[i] = make_label(s_list_row[i], &lv_font_montserrat_12, HEX_TEXT);
        lv_label_set_long_mode(s_list_lbl[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(s_list_lbl[i], 336);
        lv_label_set_text_static(s_list_lbl[i], s_list_buf[i]);
    }
    blk_add(BASHCAT_PAGE_I2C, list);

    lv_obj_t *rtc = make_panel(page);
    lv_obj_set_pos(rtc, 420, 228);
    lv_obj_set_size(rtc, 364, 188);
    lv_obj_t *rt = make_label(rtc, &lv_font_montserrat_12, HEX_MUTED);
    lv_label_set_text_static(rt, "RTC  " BULLET "  PCF85063");
    lv_obj_set_pos(rt, 16, 14);
    roll_init(&s_rtc_roll, rtc, &lv_font_montserrat_48, HEX_TEXT, 332, 52, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(s_rtc_roll.clip, 16, 58);
    roll_set(&s_rtc_roll, "--", false);
    s_rtc_date = make_label(rtc, &lv_font_montserrat_16, HEX_DIM);
    lv_obj_set_width(s_rtc_date, 332);
    lv_obj_set_style_text_align(s_rtc_date, LV_TEXT_ALIGN_CENTER, 0);
    snprintf(s_rtc_date_buf, sizeof s_rtc_date_buf, "--");
    lv_label_set_text_static(s_rtc_date, s_rtc_date_buf);
    lv_obj_set_pos(s_rtc_date, 16, 124);
    blk_add(BASHCAT_PAGE_I2C, rtc);
}

static const char *can_name(io_can_state_t st)
{
    switch (st) {
    case IO_CAN_RUNNING:
        return "RUNNING";
    case IO_CAN_ERR_PASSIVE:
        return "ERR PASSIVE";
    case IO_CAN_BUS_OFF:
        return "BUS OFF";
    case IO_CAN_RECOVERING:
        return "RECOVERING";
    default:
        return "STOPPED";
    }
}

static uint32_t can_color(io_can_state_t st)
{
    switch (st) {
    case IO_CAN_RUNNING:
        return HEX_ACCENT;
    case IO_CAN_ERR_PASSIVE:
    case IO_CAN_RECOVERING:
        return HEX_WARN;
    case IO_CAN_BUS_OFF:
        return HEX_CRIT;
    default:
        return HEX_MUTED;
    }
}

static void format_frame(char *buf, size_t n, const io_can_frame_t *fr)
{
    char hexb[40];
    int h = 0;
    hexb[0] = '\0';
    uint8_t dlc = fr->dlc > 8 ? 8 : fr->dlc;
    for (uint8_t i = 0; i < dlc; i++) {
        int w = snprintf(hexb + h, sizeof hexb - (size_t)h, h ? " %02X" : "%02X", (unsigned)fr->data[i]);
        if (w <= 0) {
            break;
        }
        h += w;
    }
    if (fr->ext) {
        snprintf(buf, n, "%08X  " BULLET "  %u  " BULLET "  %s", (unsigned)fr->id, (unsigned)dlc, hexb);
    } else {
        snprintf(buf, n, "%03X  " BULLET "  %u  " BULLET "  %s", (unsigned)(fr->id & 0x7FFu), (unsigned)dlc,
                 hexb);
    }
}

static void can_write_rows(const io_state_t *st)
{
    for (int i = 0; i < UI_CAN_ROWS; i++) {
        if (i < st->can_rx_n) {
            format_frame(s_can_txt[i], sizeof s_can_txt[i], &st->can_rx[i]);
        } else {
            s_can_txt[i][0] = '\0';
        }
        lv_label_set_text_static(s_can_lbl[i], s_can_txt[i]);
    }
}

static void can_place(bool anim)
{
    for (int i = 0; i < UI_CAN_ROWS; i++) {
        lv_coord_t dest = (lv_coord_t)(i * CAN_ROW_H);
        if (!anim) {
            lv_obj_set_y(s_can_row[i], dest);
        } else {
            lv_obj_set_y(s_can_row[i], (lv_coord_t)(dest - CAN_ROW_H));
            anim_i32(s_can_row[i], y_exec, dest - CAN_ROW_H, dest, UI_ROW_MS, lv_anim_path_ease_out, 0, NULL,
                     NULL);
        }
    }
}

static void flash_back(lv_anim_t *a)
{
    (void)a;
    lv_obj_set_style_bg_color(s_can_row[0], hex(HEX_PANEL), 0);
}

static void can_flash(void)
{
    lv_obj_set_style_bg_color(s_can_row[0], hex(HEX_ACCENT), 0);
    anim_i32(s_can_row[0], noop_exec, 0, 1, 180, lv_anim_path_linear, 0, flash_back, NULL);
}

static void build_can(lv_obj_t *page)
{
    lv_obj_t *bar = lv_obj_create(page);
    make_plain(bar);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(bar, 0, 4);
    lv_obj_set_size(bar, 800, 44);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    seg_build(&s_can_seg, bar, 16, 4, 5, 64, k_bitrate_lbl);
    wire_seg_hits(&s_can_seg, on_can_seg);
    lv_obj_t *ll = make_label(bar, &lv_font_montserrat_12, HEX_MUTED);
    lv_label_set_text_static(ll, "LISTEN");
    lv_obj_set_pos(ll, 348, 4);
    s_listen = lv_switch_create(bar);
    lv_obj_set_pos(s_listen, 348, 18);
    lv_obj_set_size(s_listen, 48, 22);
    lv_obj_set_style_bg_color(s_listen, hex(HEX_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_listen, hex(HEX_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_listen, hex(HEX_TEXT), LV_PART_KNOB);
    lv_obj_set_style_anim_time(s_listen, 180, 0);
    lv_obj_add_event_cb(s_listen, on_listen, LV_EVENT_VALUE_CHANGED, NULL);
    s_start_btn = lv_obj_create(bar);
    style_btn(s_start_btn);
    lv_obj_set_pos(s_start_btn, 496, 4);
    lv_obj_set_size(s_start_btn, 136, 36);
    s_start_fade.lbl = make_label(s_start_btn, &lv_font_montserrat_16, HEX_TEXT);
    lv_obj_center(s_start_fade.lbl);
    fade_set(&s_start_fade, "START");
    lv_obj_add_event_cb(s_start_btn, on_start, LV_EVENT_CLICKED, NULL);
    s_send_btn = lv_obj_create(bar);
    style_btn(s_send_btn);
    lv_obj_set_pos(s_send_btn, 644, 4);
    lv_obj_set_size(s_send_btn, 140, 36);
    btn_label(s_send_btn, "SEND 0x123");
    lv_obj_add_state(s_send_btn, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_send_btn, on_send, LV_EVENT_CLICKED, NULL);
    s_send_bar = lv_obj_create(s_send_btn);
    make_plain(s_send_bar);
    lv_obj_clear_flag(s_send_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_send_bar, 0, 34);
    lv_obj_set_size(s_send_bar, 0, 2);
    lv_obj_set_style_bg_color(s_send_bar, hex(HEX_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_send_bar, LV_OPA_COVER, 0);
    blk_add(BASHCAT_PAGE_CAN, bar);

    lv_obj_t *stat = lv_obj_create(page);
    make_plain(stat);
    lv_obj_clear_flag(stat, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(stat, 16, 56);
    lv_obj_set_size(stat, 768, 28);
    lv_obj_set_style_bg_opa(stat, LV_OPA_TRANSP, 0);
    s_can_lamp = lv_obj_create(stat);
    make_plain(s_can_lamp);
    lv_obj_clear_flag(s_can_lamp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_can_lamp, 12, 12);
    lv_obj_set_pos(s_can_lamp, 0, 6);
    lv_obj_set_style_radius(s_can_lamp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_can_lamp, hex(HEX_MUTED), 0);
    lv_obj_set_style_bg_opa(s_can_lamp, LV_OPA_COVER, 0);
    lv_obj_set_style_transition(s_can_lamp, &s_color_tr, 0);
    s_can_fade.lbl = make_label(stat, &lv_font_montserrat_16, HEX_MUTED);
    lv_obj_set_pos(s_can_fade.lbl, 22, 2);
    lv_obj_set_style_transition(s_can_fade.lbl, &s_color_tr, 0);
    fade_set(&s_can_fade, "STOPPED");
    blk_add(BASHCAT_PAGE_CAN, stat);

    lv_obj_t *tiles = lv_obj_create(page);
    make_plain(tiles);
    lv_obj_clear_flag(tiles, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(tiles, 16, 90);
    lv_obj_set_size(tiles, 768, 68);
    lv_obj_set_style_bg_opa(tiles, LV_OPA_TRANSP, 0);
    static const char *const kn[5] = {"RX", "TX", "ERR", "TEC", "REC"};
    for (int i = 0; i < 5; i++) {
        lv_obj_t *t = make_panel(tiles);
        lv_obj_set_pos(t, (lv_coord_t)(i * 156), 0);
        lv_obj_set_size(t, 148, 68);
        lv_obj_t *k = make_label(t, &lv_font_montserrat_12, HEX_MUTED);
        lv_label_set_text(k, kn[i]);
        lv_obj_set_pos(k, 12, 8);
        s_can_num[i].lbl = make_label(t, &lv_font_montserrat_20, HEX_TEXT);
        s_can_num[i].shown = 0;
        s_can_num[i].target = 0;
        snprintf(s_can_num[i].buf, sizeof s_can_num[i].buf, "0");
        lv_label_set_text_static(s_can_num[i].lbl, s_can_num[i].buf);
        lv_obj_set_pos(s_can_num[i].lbl, 12, 30);
    }
    blk_add(BASHCAT_PAGE_CAN, tiles);

    lv_obj_t *list = make_panel(page);
    lv_obj_set_pos(list, 16, 166);
    lv_obj_set_size(list, 768, 188);
    lv_obj_t *clip = lv_obj_create(list);
    make_plain(clip);
    lv_obj_clear_flag(clip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(clip, 8, 6);
    lv_obj_set_size(clip, 752, UI_CAN_ROWS * CAN_ROW_H);
    lv_obj_set_style_bg_opa(clip, LV_OPA_TRANSP, 0);
    for (int i = 0; i < UI_CAN_ROWS; i++) {
        s_can_row[i] = lv_obj_create(clip);
        make_plain(s_can_row[i]);
        lv_obj_clear_flag(s_can_row[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(s_can_row[i], 0, (lv_coord_t)(i * CAN_ROW_H));
        lv_obj_set_size(s_can_row[i], 752, CAN_ROW_H);
        lv_obj_set_style_bg_color(s_can_row[i], hex(HEX_PANEL), 0);
        lv_obj_set_style_bg_opa(s_can_row[i], LV_OPA_COVER, 0);
        lv_obj_set_style_transition(s_can_row[i], &s_color_tr, 0);
        s_can_lbl[i] = make_label(s_can_row[i], &lv_font_montserrat_12, HEX_TEXT);
        lv_obj_set_pos(s_can_lbl[i], 8, 3);
        lv_obj_set_width(s_can_lbl[i], 730);
        lv_label_set_long_mode(s_can_lbl[i], LV_LABEL_LONG_CLIP);
        lv_label_set_text_static(s_can_lbl[i], s_can_txt[i]);
    }
    s_can_ghost = make_label(clip, &lv_font_montserrat_12, HEX_DIM);
    lv_obj_set_width(s_can_ghost, 730);
    lv_label_set_long_mode(s_can_ghost, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(s_can_ghost, 8, UI_CAN_ROWS * CAN_ROW_H);
    lv_obj_add_flag(s_can_ghost, LV_OBJ_FLAG_HIDDEN);
    blk_add(BASHCAT_PAGE_CAN, list);

    lv_obj_t *spark = make_panel(page);
    lv_obj_set_pos(spark, 16, 360);
    lv_obj_set_size(spark, 300, 60);
    lv_obj_t *sp = make_label(spark, &lv_font_montserrat_12, HEX_MUTED);
    lv_label_set_text_static(sp, "frames / s");
    lv_obj_set_pos(sp, 10, 4);
    s_spark = make_chart(spark, 90, 6, 198, 48, 20, 0, 10, &s_spark_ser);
    blk_add(BASHCAT_PAGE_CAN, spark);
}

static void rs_write(const io_state_t *st)
{
    for (int i = 0; i < UI_RS_ROWS; i++) {
        if (i < st->rs485_line_n) {
            const io_line_t *ln = &st->rs485_line[i];
            const char *show = ln->text;
            if (strchr(ln->text, '.') != NULL && ln->hex[0] != '\0') {
                show = ln->hex;
            }
            snprintf(s_rs_txt[i], sizeof s_rs_txt[i], "%s", show);
            lv_label_set_text(s_rs_mark[i], ln->is_tx ? "<" : ">");
            lv_obj_set_style_text_color(s_rs_mark[i], hex(ln->is_tx ? HEX_ACCENT : HEX_MUTED), 0);
        } else {
            s_rs_txt[i][0] = '\0';
            lv_label_set_text(s_rs_mark[i], "");
        }
        lv_label_set_text_static(s_rs_lbl[i], s_rs_txt[i]);
    }
}

static void rs_place(bool anim)
{
    for (int i = 0; i < UI_RS_ROWS; i++) {
        lv_coord_t dest = (lv_coord_t)(i * RS_ROW_H);
        lv_coord_t from = (lv_coord_t)(dest + RS_ROW_H);
        if (!anim) {
            lv_obj_set_y(s_rs_row[i], dest);
        } else {
            lv_obj_set_y(s_rs_row[i], from);
            anim_i32(s_rs_row[i], y_exec, from, dest, UI_ROW_MS, lv_anim_path_ease_out, 0, NULL, NULL);
        }
    }
}

static void lamp_dim(lv_anim_t *a)
{
    lv_obj_t *lamp = (lv_obj_t *)a->user_data;
    lv_obj_set_style_bg_color(lamp, hex(HEX_LINE), 0);
    lv_obj_set_style_bg_opa(lamp, LV_OPA_40, 0);
}

static void lamp_flash(lv_obj_t *lamp)
{
    lv_obj_set_style_bg_color(lamp, hex(HEX_ACCENT), 0);
    lv_obj_set_style_bg_opa(lamp, LV_OPA_COVER, 0);
    anim_i32(lamp, bg_opa_exec, LV_OPA_COVER, LV_OPA_40, UI_LAMP_MS, lv_anim_path_ease_out, 0, lamp_dim, lamp);
}

static void build_rs(lv_obj_t *page)
{
    lv_obj_t *bar = lv_obj_create(page);
    make_plain(bar);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(bar, 0, 4);
    lv_obj_set_size(bar, 800, 44);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    seg_build(&s_rs_seg, bar, 16, 4, 4, 92, k_baud_lbl);
    seg_select(&s_rs_seg, 3, false);
    wire_seg_hits(&s_rs_seg, on_baud);
    lv_obj_t *rl = make_label(bar, &lv_font_montserrat_12, HEX_MUTED);
    lv_label_set_text_static(rl, "RX");
    lv_obj_set_pos(rl, 410, 12);
    s_rx_lamp = lv_obj_create(bar);
    make_plain(s_rx_lamp);
    lv_obj_clear_flag(s_rx_lamp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_rx_lamp, 436, 14);
    lv_obj_set_size(s_rx_lamp, 14, 14);
    lv_obj_set_style_radius(s_rx_lamp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_rx_lamp, hex(HEX_LINE), 0);
    lv_obj_set_style_bg_opa(s_rx_lamp, LV_OPA_40, 0);
    lv_obj_t *tl = make_label(bar, &lv_font_montserrat_12, HEX_MUTED);
    lv_label_set_text_static(tl, "TX");
    lv_obj_set_pos(tl, 462, 12);
    s_tx_lamp = lv_obj_create(bar);
    make_plain(s_tx_lamp);
    lv_obj_clear_flag(s_tx_lamp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_tx_lamp, 488, 14);
    lv_obj_set_size(s_tx_lamp, 14, 14);
    lv_obj_set_style_radius(s_tx_lamp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_tx_lamp, hex(HEX_LINE), 0);
    lv_obj_set_style_bg_opa(s_tx_lamp, LV_OPA_40, 0);
    static const char *rn[2] = {"RX bytes", "TX bytes"};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *cap = make_label(bar, &lv_font_montserrat_12, HEX_MUTED);
        lv_label_set_text(cap, rn[i]);
        lv_obj_set_pos(cap, (lv_coord_t)(530 + i * 120), 2);
        s_rs_num[i].lbl = make_label(bar, &lv_font_montserrat_16, HEX_TEXT);
        snprintf(s_rs_num[i].buf, sizeof s_rs_num[i].buf, "0");
        lv_label_set_text_static(s_rs_num[i].lbl, s_rs_num[i].buf);
        lv_obj_set_pos(s_rs_num[i].lbl, (lv_coord_t)(530 + i * 120), 18);
    }
    blk_add(BASHCAT_PAGE_RS485, bar);

    lv_obj_t *term = make_panel(page);
    lv_obj_set_pos(term, 16, 56);
    lv_obj_set_size(term, 768, 292);
    lv_obj_t *clip = lv_obj_create(term);
    make_plain(clip);
    lv_obj_clear_flag(clip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(clip, 12, 12);
    lv_obj_set_size(clip, 744, UI_RS_ROWS * RS_ROW_H);
    lv_obj_set_style_bg_opa(clip, LV_OPA_TRANSP, 0);
    for (int i = 0; i < UI_RS_ROWS; i++) {
        s_rs_row[i] = lv_obj_create(clip);
        make_plain(s_rs_row[i]);
        lv_obj_clear_flag(s_rs_row[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(s_rs_row[i], 0, (lv_coord_t)(i * RS_ROW_H));
        lv_obj_set_size(s_rs_row[i], 744, RS_ROW_H);
        lv_obj_set_style_bg_opa(s_rs_row[i], LV_OPA_TRANSP, 0);
        s_rs_mark[i] = make_label(s_rs_row[i], &lv_font_montserrat_16, HEX_MUTED);
        lv_label_set_text(s_rs_mark[i], "");
        lv_obj_set_pos(s_rs_mark[i], 0, 4);
        s_rs_lbl[i] = make_label(s_rs_row[i], &lv_font_montserrat_16, HEX_TEXT);
        lv_obj_set_pos(s_rs_lbl[i], 22, 4);
        lv_obj_set_width(s_rs_lbl[i], 700);
        lv_label_set_long_mode(s_rs_lbl[i], LV_LABEL_LONG_CLIP);
        lv_label_set_text_static(s_rs_lbl[i], s_rs_txt[i]);
    }
    s_rs_ghost = make_label(clip, &lv_font_montserrat_16, HEX_DIM);
    lv_obj_set_width(s_rs_ghost, 700);
    lv_label_set_long_mode(s_rs_ghost, LV_LABEL_LONG_CLIP);
    lv_obj_add_flag(s_rs_ghost, LV_OBJ_FLAG_HIDDEN);
    blk_add(BASHCAT_PAGE_RS485, term);

    lv_obj_t *keys = lv_obj_create(page);
    make_plain(keys);
    lv_obj_clear_flag(keys, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(keys, 16, 356);
    lv_obj_set_size(keys, 768, 60);
    lv_obj_set_style_bg_opa(keys, LV_OPA_TRANSP, 0);
    static const char *const bt[3] = {"PING", "HELLO", "0x55 AA"};
    static const lv_coord_t bw[3] = {168, 180, 160};
    lv_coord_t x = 0;
    lv_event_cb_t cbs[3] = {on_ping, on_hello, on_aa};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_obj_create(keys);
        style_btn(b);
        lv_obj_set_pos(b, x, 4);
        lv_obj_set_size(b, bw[i], 52);
        btn_label(b, bt[i]);
        lv_obj_add_event_cb(b, cbs[i], LV_EVENT_CLICKED, NULL);
        x = (lv_coord_t)(x + bw[i] + 16);
    }
    blk_add(BASHCAT_PAGE_RS485, keys);
}

static void apply_io(const io_state_t *st)
{
    if (!s_have_di) {
        s_have_di = true;
        s_prev_di = st->di;
    } else if (st->di != s_prev_di) {
        for (int i = 0; i < 2; i++) {
            uint8_t m = (uint8_t)(1u << i);
            if ((st->di ^ s_prev_di) & m) {
                anim_i32(&s_pulse[i], pulse_exec, 0, 1000, UI_PULSE_MS, lv_anim_path_ease_out, 0, NULL, NULL);
            }
        }
        s_prev_di = st->di;
    }
    for (int i = 0; i < 2; i++) {
        lamp_set(s_lamp[i], (st->di & (uint8_t)(1u << i)) != 0);
        char ch[24];
        snprintf(ch, sizeof ch, "changes: %u", (unsigned)st->di_change_count[i]);
        roll_set(&s_changes[i], ch, true);
        int actual = (st->do_ >> i) & 1;
        if (s_do_want[i] >= 0) {
            if (actual == s_do_want[i]) {
                s_do_want[i] = -1;
                roll_set(&s_do_roll[i], actual ? "ON" : "OFF", true);
            } else {
                roll_set(&s_do_roll[i], "...", true);
            }
        } else {
            bool checked = lv_obj_has_state(s_sw[i], LV_STATE_CHECKED);
            if (checked != (actual != 0)) {
                s_do_sync = true;
                if (actual) {
                    lv_obj_add_state(s_sw[i], LV_STATE_CHECKED);
                } else {
                    lv_obj_clear_state(s_sw[i], LV_STATE_CHECKED);
                }
                s_do_sync = false;
            }
            roll_set(&s_do_roll[i], actual ? "ON" : "OFF", true);
        }
        if (s_di_chart[i] && s_di_ser[i]) {
            lv_chart_set_next_value(s_di_chart[i], s_di_ser[i], (st->di & (uint8_t)(1u << i)) ? 100 : 0);
        }
    }
}

static void move_cursor(int addr)
{
    if (addr < 0 || addr > 127 || s_cell[addr] == NULL) {
        return;
    }
    lv_obj_set_pos(s_cursor, lv_obj_get_x(s_cell[addr]), lv_obj_get_y(s_cell[addr]));
    lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_cursor);
    if (s_hi_addr >= 0 && s_hi_addr != addr && s_hi_addr < 128 &&
        (s_popped[s_hi_addr >> 3] & (uint8_t)(1u << (s_hi_addr & 7))) == 0) {
        lv_obj_set_style_bg_opa(s_cell[s_hi_addr], LV_OPA_TRANSP, 0);
    }
    s_hi_addr = addr;
    if ((s_popped[addr >> 3] & (uint8_t)(1u << (addr & 7))) == 0) {
        lv_obj_set_style_bg_color(s_cell[addr], hex(HEX_LINE), 0);
        lv_obj_set_style_bg_opa(s_cell[addr], LV_OPA_COVER, 0);
    }
}

static void apply_i2c(const io_state_t *st)
{
    if (st->scan_busy && !s_scan_busy_prev) {
        reset_i2c_visuals();
    }
    s_scan_busy_prev = st->scan_busy;
    if (st->scan_busy) {
        lv_obj_add_state(s_scan_btn, LV_STATE_DISABLED);
        int idx = (st->scan_progress * 111) / 100;
        if (idx > 111) {
            idx = 111;
        }
        move_cursor(0x08 + idx);
    } else {
        lv_obj_clear_state(s_scan_btn, LV_STATE_DISABLED);
        lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
        if (s_hi_addr >= 0 && s_hi_addr < 128 &&
            (s_popped[s_hi_addr >> 3] & (uint8_t)(1u << (s_hi_addr & 7))) == 0) {
            lv_obj_set_style_bg_opa(s_cell[s_hi_addr], LV_OPA_TRANSP, 0);
        }
        s_hi_addr = -1;
    }
    for (unsigned addr = 0; addr < 128; addr++) {
        bool on = io_found_get(st, addr);
        bool was = (s_popped[addr >> 3] & (uint8_t)(1u << (addr & 7))) != 0;
        if (on && !was) {
            s_popped[addr >> 3] = (uint8_t)(s_popped[addr >> 3] | (uint8_t)(1u << (addr & 7)));
            pop_cell((int)addr);
            note_found(addr);
        }
    }
    if (st->rtc_ok) {
        roll_set(&s_rtc_roll, st->rtc_hms, true);
        if (strcmp(s_rtc_date_buf, st->rtc_date) != 0) {
            snprintf(s_rtc_date_buf, sizeof s_rtc_date_buf, "%s", st->rtc_date);
            lv_label_set_text_static(s_rtc_date, s_rtc_date_buf);
        }
    } else {
        roll_set(&s_rtc_roll, "--", true);
        if (strcmp(s_rtc_date_buf, "--") != 0) {
            snprintf(s_rtc_date_buf, sizeof s_rtc_date_buf, "--");
            lv_label_set_text_static(s_rtc_date, s_rtc_date_buf);
        }
    }
}

static void apply_can(const io_state_t *st)
{
    bool running = st->can_state != IO_CAN_STOPPED;
    s_can_run = running;
    if (running) {
        lv_obj_clear_state(s_send_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_send_btn, LV_STATE_DISABLED);
    }
    fade_set(&s_start_fade, running ? "STOP" : "START");
    if ((int)st->can_state != s_can_shown) {
        s_can_shown = (int)st->can_state;
        uint32_t col = can_color(st->can_state);
        lv_obj_set_style_bg_color(s_can_lamp, hex(col), 0);
        lv_obj_set_style_text_color(s_can_fade.lbl, hex(col), 0);
        fade_set(&s_can_fade, can_name(st->can_state));
    }
    num_to(&s_can_num[0], st->can_rx_count);
    num_to(&s_can_num[1], st->can_tx_count);
    num_to(&s_can_num[2], st->can_err_count);
    num_to(&s_can_num[3], st->can_tec);
    num_to(&s_can_num[4], st->can_rec);

    if (!s_can_seq_init) {
        s_can_seq_init = true;
        s_can_seq_seen = st->can_rx_seq;
        can_write_rows(st);
        can_place(false);
    } else if (st->can_rx_seq != s_can_seq_seen) {
        uint32_t d = st->can_rx_seq - s_can_seq_seen;
        s_can_seq_seen = st->can_rx_seq;
        snprintf(s_can_ghost_buf, sizeof s_can_ghost_buf, "%s", s_can_txt[UI_CAN_ROWS - 1]);
        lv_label_set_text(s_can_ghost, s_can_ghost_buf);
        lv_obj_clear_flag(s_can_ghost, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_can_ghost, 8, (UI_CAN_ROWS - 1) * CAN_ROW_H);
        lv_obj_set_style_opa(s_can_ghost, LV_OPA_COVER, 0);
        can_write_rows(st);
        if (d == 1) {
            can_place(true);
            can_flash();
            anim_i32(s_can_ghost, y_exec, (UI_CAN_ROWS - 1) * CAN_ROW_H, UI_CAN_ROWS * CAN_ROW_H, UI_ROW_MS,
                     lv_anim_path_ease_out, 0, NULL, NULL);
            anim_i32(s_can_ghost, fade_exec, LV_OPA_COVER, 0, UI_ROW_MS, lv_anim_path_ease_out, 0, opa_hide,
                     NULL);
        } else {
            can_place(false);
            lv_obj_add_flag(s_can_ghost, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_local_style_prop(s_can_ghost, LV_STYLE_OPA, 0);
        }
    }

    if (!s_spark_init) {
        s_spark_init = true;
        s_spark_ms = lv_tick_get();
        s_spark_rx = st->can_rx_count;
    } else if (lv_tick_elaps(s_spark_ms) >= 1000) {
        uint32_t d = st->can_rx_count - s_spark_rx;
        s_spark_rx = st->can_rx_count;
        s_spark_ms = lv_tick_get();
        lv_coord_t y = (lv_coord_t)(d > 10u ? 10u : d);
        lv_chart_set_next_value(s_spark, s_spark_ser, y);
    }
}

static void apply_rs(const io_state_t *st)
{
    num_to(&s_rs_num[0], st->rs485_rx_bytes);
    num_to(&s_rs_num[1], st->rs485_tx_bytes);
    if (!s_rs_seq_init) {
        s_rs_seq_init = true;
        s_rx_seq_seen = st->rs485_rx_seq;
        s_tx_seq_seen = st->rs485_tx_seq;
    } else {
        if (st->rs485_rx_seq != s_rx_seq_seen) {
            s_rx_seq_seen = st->rs485_rx_seq;
            lamp_flash(s_rx_lamp);
        }
        if (st->rs485_tx_seq != s_tx_seq_seen) {
            s_tx_seq_seen = st->rs485_tx_seq;
            lamp_flash(s_tx_lamp);
        }
    }
    if (!s_log_init) {
        s_log_init = true;
        s_log_seen = st->rs485_log_seq;
        rs_write(st);
        rs_place(false);
    } else if (st->rs485_log_seq != s_log_seen) {
        uint32_t d = st->rs485_log_seq - s_log_seen;
        s_log_seen = st->rs485_log_seq;
        snprintf(s_rs_ghost_buf, sizeof s_rs_ghost_buf, "%s", s_rs_txt[0]);
        lv_label_set_text(s_rs_ghost, s_rs_ghost_buf);
        lv_obj_clear_flag(s_rs_ghost, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_rs_ghost, 22, 4);
        lv_obj_set_style_opa(s_rs_ghost, LV_OPA_COVER, 0);
        rs_write(st);
        if (d == 1) {
            rs_place(true);
            anim_i32(s_rs_ghost, y_exec, 4, 4 - RS_ROW_H, UI_ROW_MS, lv_anim_path_ease_out, 0, NULL, NULL);
            anim_i32(s_rs_ghost, fade_exec, LV_OPA_COVER, 0, UI_ROW_MS, lv_anim_path_ease_out, 0, opa_hide,
                     NULL);
        } else {
            rs_place(false);
            lv_obj_add_flag(s_rs_ghost, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_local_style_prop(s_rs_ghost, LV_STYLE_OPA, 0);
        }
    }
}

void bashcat_ui_v3_build(lv_obj_t *io, lv_obj_t *i2c, lv_obj_t *can, lv_obj_t *rs485)
{
    lv_style_transition_dsc_init(&s_press_tr, k_press_props, lv_anim_path_ease_out, UI_PRESS_MS, 0, NULL);
    lv_style_transition_dsc_init(&s_color_tr, k_color_props, lv_anim_path_ease_out, UI_SEG_MS, 0, NULL);
    build_io(io);
    build_i2c(i2c);
    build_can(can);
    build_rs(rs485);
}

void bashcat_ui_v3_enter(int page)
{
    int i = page - BASHCAT_PAGE_IO;
    if (i < 0 || i > 3) {
        return;
    }
    blk_prime(i);
    blk_play(i);
}

void bashcat_ui_v3_apply(const io_state_t *st)
{
    if (st == NULL) {
        return;
    }
    apply_io(st);
    apply_i2c(st);
    apply_can(st);
    apply_rs(st);
}
