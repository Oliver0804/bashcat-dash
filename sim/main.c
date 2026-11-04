/*
 * Headless LVGL 8.4 renderer. 800x480 RGB565, 10 ms simulated steps.
 * Does not link SDL. Frames are written with stb_image_write.
 *
 * The pointer is a script: each stroke is a press held from t0 to t1,
 * moving dx pixels every 10 ms. A step of 10 px keeps |vect| above
 * LVGL's gesture_min_velocity (3) so a swipe sums past gesture_limit (50).
 * The first sample of a press has a zero vector; the gesture lands ~60 ms
 * later. Swipes stay on y=4, above the header widgets, so the screen
 * itself receives the gesture.
 */

#include "bashcat_ui.h"
#include "esp_timer.h"

#include "lvgl.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define HOR 800
#define VER 480
#define SIM_END_MS 26500
#define SIM_STEP_MS 10

static lv_color_t s_draw[HOR * VER];
static uint16_t s_fb[HOR * VER];
static uint8_t s_rgb[HOR * VER * 3];
static int s_pressed;
static int s_px;
static int s_py;

/* Pixels flushed between two lv_timer_handler calls. The DASH window is
 * t=[3000, 13000): intro and the self-test are over, the IO click has
 * not started. Zeros (no flush) are counted separately from flushed frames. */
static uint32_t s_frame_px;
static uint64_t s_win_px;
static uint32_t s_win_flushed;
static uint32_t s_win_steps;

typedef struct {
    int t0;
    int t1;
    int x;
    int y;
    int dx;
} stroke_t;

/*
 * Clicks are stationary (dx = 0), so they do not sum a gesture.
 * Dot centers are the settled positions of the next page indicator.
 * DO0 is the left switch on the IO output card.
 */
static const stroke_t k_strokes[] = {
    {13000, 13100, 382, 21, 0},  /* dash -> io */
    {13800, 13900, 514, 181, 0}, /* DO0 */
    {16000, 16100, 394, 21, 0},  /* io -> i2c */
    {16800, 16900, 82, 78, 0},   /* SCAN */
    {19000, 19100, 406, 21, 0},  /* i2c -> can */
    {19800, 19900, 176, 78, 0},  /* 250k */
    {20600, 20700, 564, 78, 0},  /* START */
    {22320, 22420, 714, 78, 0},  /* SEND */
    {23200, 23300, 418, 21, 0},  /* can -> rs485 */
    {24000, 24100, 100, 438, 0}, /* PING */
    {25000, 25100, 430, 21, 0},  /* rs485 -> system */
};

typedef struct {
    int t;
    const char *name;
} shot_t;

static const shot_t k_v3[] = {
    {5000, "dash"},
    {13260, "io_slide"},
    {13600, "io"},
    {13960, "do_mid"},
    {14200, "do_high"},
    {15200, "di_pulse"},
    {15600, "di_settled"},
    {16260, "i2c_slide"},
    {16600, "i2c"},
    {17500, "scan_mid"},
    {18220, "scan_pop"},
    {18600, "scan_done"},
    {19260, "can_slide"},
    {19600, "can_stopped"},
    {20060, "bitrate_mid"},
    {20300, "bitrate_250k"},
    {21120, "can_rx_mid"},
    {21550, "can_running"},
    {22600, "send_mid"},
    {23050, "send_done"},
    {23460, "rs485_slide"},
    {23800, "rs485"},
    {24220, "ping_mid"},
    {24650, "ping_done"},
    {25420, "system_mid"},
    {26100, "system"},
};

static const int k_boot[] = {300, 900, 1600, 2200, 2700, 3200, 3800, 4500, 6000, 7300, 12000};

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    int32_t w = (int32_t)(area->x2 - area->x1 + 1);
    int32_t h = (int32_t)(area->y2 - area->y1 + 1);
    if (w > 0 && h > 0) {
        s_frame_px += (uint32_t)w * (uint32_t)h;
    }
    for (int32_t y = area->y1; y <= area->y2; y++) {
        uint16_t *row = &s_fb[(size_t)y * HOR + (size_t)area->x1];
        for (int32_t x = 0; x < w; x++) {
            row[x] = color_p[x].full;
        }
        color_p += w;
    }
    lv_disp_flush_ready(drv);
}

static void pointer_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    data->point.x = (lv_coord_t)s_px;
    data->point.y = (lv_coord_t)s_py;
    data->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = false;
}

static void apply_pointer(int t)
{
    s_pressed = 0;
    for (size_t i = 0; i < sizeof(k_strokes) / sizeof(k_strokes[0]); i++) {
        const stroke_t *s = &k_strokes[i];
        if (t >= s->t0 && t < s->t1) {
            s_pressed = 1;
            s_px = s->x + s->dx * ((t - s->t0) / SIM_STEP_MS);
            s_py = s->y;
            return;
        }
    }
}

static void ensure_dir(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        perror(path);
        exit(1);
    }
}

static void write_png(const char *path)
{
    for (int i = 0; i < HOR * VER; i++) {
        uint16_t c = s_fb[i];
        uint8_t r = (uint8_t)((c >> 11) & 0x1F);
        uint8_t g = (uint8_t)((c >> 5) & 0x3F);
        uint8_t b = (uint8_t)(c & 0x1F);
        s_rgb[i * 3 + 0] = (uint8_t)((r << 3) | (r >> 2));
        s_rgb[i * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
        s_rgb[i * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
    }
    if (stbi_write_png(path, HOR, VER, 3, s_rgb, HOR * 3) == 0) {
        fprintf(stderr, "png write failed: %s\n", path);
        exit(1);
    }
}

static const char *v3_name(int t)
{
    for (size_t i = 0; i < sizeof(k_v3) / sizeof(k_v3[0]); i++) {
        if (k_v3[i].t == t) {
            return k_v3[i].name;
        }
    }
    return NULL;
}

static int is_boot_shot(int t)
{
    for (size_t i = 0; i < sizeof(k_boot) / sizeof(k_boot[0]); i++) {
        if (k_boot[i] == t) {
            return 1;
        }
    }
    return 0;
}

static void capture(int t)
{
    char path[160];
    const char *event = v3_name(t);
    if (t <= 12000 && (t % 40) == 0) {
        snprintf(path, sizeof(path), "out/seq/frame_%05d.png", t / 40);
        write_png(path);
    }
    if ((t % 100) == 0) {
        snprintf(path, sizeof(path), "out/v3seq/frame_%05d.png", t / 100);
        write_png(path);
    }
    if (event) {
        snprintf(path, sizeof(path), "out/v3_%05dms_%s.png", t, event);
        write_png(path);
        printf("%s\n", path);
        fflush(stdout);
    }
    if (is_boot_shot(t)) {
        snprintf(path, sizeof(path), "out/frame_%05dms.png", t);
        write_png(path);
    }
}

static void step(int t)
{
    s_frame_px = 0;
    sim_timer_set_us((int64_t)t * 1000);
    apply_pointer(t);
    if (t > 0) {
        lv_tick_inc(SIM_STEP_MS);
    }
    lv_timer_handler();
    if (t == 0) {
        lv_refr_now(NULL);
    }
    if (t >= 3000 && t < 13000) {
        s_win_steps++;
        if (s_frame_px > 0) {
            s_win_px += s_frame_px;
            s_win_flushed++;
        }
    }
    capture(t);
}

int main(void)
{
    ensure_dir("out");
    ensure_dir("out/seq");
    ensure_dir("out/v3seq");

    lv_init();

    static lv_disp_draw_buf_t disp_buf;
    static lv_disp_drv_t disp_drv;
    lv_disp_draw_buf_init(&disp_buf, s_draw, NULL, HOR * VER);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = HOR;
    disp_drv.ver_res = VER;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &disp_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = pointer_read;
    lv_indev_drv_register(&indev_drv);

    bashcat_ui_start();

    for (int t = 0; t <= SIM_END_MS; t += SIM_STEP_MS) {
        step(t);
        if (t > 0 && (t % 5000) == 0) {
            printf("t=%d\n", t);
            fflush(stdout);
        }
    }
    {
        uint64_t avg_flushed = s_win_flushed ? s_win_px / s_win_flushed : 0;
        uint64_t avg_all = s_win_steps ? s_win_px / s_win_steps : 0;
        printf("PERF window=[3000,13000) steps=%u flushed=%u px_sum=%llu avg_flushed=%llu avg_all=%llu\n",
               s_win_steps, s_win_flushed, (unsigned long long)s_win_px,
               (unsigned long long)avg_flushed, (unsigned long long)avg_all);
        fflush(stdout);
    }
    return 0;
}
