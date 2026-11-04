/*
 * Peripheral backend. One task owns CH422G, the I2C scan, TWAI, and UART2.
 * The UI never calls a driver. Legacy I2C is already installed by the LCD
 * port; this file must not call i2c_driver_install or the new i2c_master API.
 *
 * CH422G IO0–IO7 share one direction bit. A DI read writes 0x24 ← 0x00, which
 * also floats the backlight, touch reset, and LCD reset. After every DI read
 * and every DO write the task puts the group back to output and rewrites the
 * shadow byte. Nothing runs between that pair except the two transactions.
 */

#include "bashcat_io.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/twai.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bashcat_io";

#define IO_I2C_PORT          I2C_NUM_0
#define IO_I2C_TICKS         pdMS_TO_TICKS(50)
#define IO_CH422_OE          0x24
#define IO_CH422_OC          0x23
#define IO_CH422_OUT         0x38
#define IO_CH422_IN          0x26
#define IO_SHADOW_INIT       0x1E
#define IO_DI0_MASK          0x01
#define IO_DI1_MASK          0x20
/* Schematic (ESP32-S3-Touch-LCD-4.3B-Sch.pdf, DI/DO block):
 *  DO: 3V3 -> 510R -> PC817 LED -> OD0/OD1. The LED only conducts when the
 *      CH422G OC pin is LOW, which turns on the NPN sink on DOUT0/DOUT1.
 *  DI: 4.7k pull-up to 3V3, PC814 transistor to GND. An active input pulls
 *      EXIO0/EXIO5 LOW.
 * Both are active-low on the expander side; the UI works in logical terms. */
#define IO_DO_ACTIVE_LOW     1
#define IO_DI_ACTIVE_LOW     1
#define IO_DI_POLL_MS        100
#define IO_DI_POLL_ENABLE    1
#define IO_RTC_ADDR          0x51
#define IO_RTC_PERIOD_MS     1000
#define IO_SCAN_FIRST        0x08
#define IO_SCAN_LAST         0x77
#define IO_SCAN_TICKS        pdMS_TO_TICKS(10)
#define IO_CAN_TX_GPIO       GPIO_NUM_15
#define IO_CAN_RX_GPIO       GPIO_NUM_16
#define IO_RS_UART           UART_NUM_2
#define IO_RS_TX_GPIO        44
#define IO_RS_RX_GPIO        43
#define IO_RS_BAUD_DEFAULT   115200
#define IO_RS_RX_BUF         1024
#define IO_LINE_MAX          32
#define IO_LINE_IDLE_US      50000
#define IO_QUEUE_LEN         16
#define IO_TASK_STACK        6144
#define IO_TASK_PRIO         5
#define IO_TASK_CORE         0

static uint8_t s_io_out = IO_SHADOW_INIT;
static uint8_t s_do_bits;
static bool s_di_valid;
static bool s_can_up;
static bool s_can_listen;
static bool s_recover_sent;
static bool s_scanning;
static uint32_t s_can_bitrate = 50000;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_mu;
static io_state_t s_st;

static uint8_t s_asm[IO_LINE_MAX];
static int s_asm_n;
static int64_t s_asm_last_us;
static int64_t s_next_di;
static int64_t s_next_rtc;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void state_lock(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
}

static void state_unlock(void)
{
    xSemaphoreGive(s_mu);
}

static void found_set(int addr)
{
    if (addr < 0 || addr >= 128) {
        return;
    }
    s_st.found[addr >> 3] = (uint8_t)(s_st.found[addr >> 3] | (uint8_t)(1u << (addr & 7)));
}

static void found_clear_all(void)
{
    memset(s_st.found, 0, sizeof s_st.found);
}

/* The two writes below are the whole body on purpose. */
static void ch422_restore(void)
{
    uint8_t oe = 0x01;
    i2c_master_write_to_device(IO_I2C_PORT, IO_CH422_OE, &oe, 1, IO_I2C_TICKS);
    i2c_master_write_to_device(IO_I2C_PORT, IO_CH422_OUT, &s_io_out, 1, IO_I2C_TICKS);
}

static void ch422_write_do(uint8_t bits)
{
    uint8_t oe = 0x01;
    i2c_master_write_to_device(IO_I2C_PORT, IO_CH422_OE, &oe, 1, IO_I2C_TICKS);
    i2c_master_write_to_device(IO_I2C_PORT, IO_CH422_OC, &bits, 1, IO_I2C_TICKS);
    i2c_master_write_to_device(IO_I2C_PORT, IO_CH422_OUT, &s_io_out, 1, IO_I2C_TICKS);
}

static void publish_di(uint8_t raw, bool ok)
{
    if (!ok) {
        return;
    }
    uint8_t di = 0;
    if (IO_DI_ACTIVE_LOW ? !(raw & IO_DI0_MASK) : (raw & IO_DI0_MASK)) {
        di = (uint8_t)(di | 0x01);
    }
    if (IO_DI_ACTIVE_LOW ? !(raw & IO_DI1_MASK) : (raw & IO_DI1_MASK)) {
        di = (uint8_t)(di | 0x02);
    }
    state_lock();
    if (s_di_valid) {
        if (((s_st.di ^ di) & 0x01) != 0) {
            s_st.di_change_count[0]++;
        }
        if (((s_st.di ^ di) & 0x02) != 0) {
            s_st.di_change_count[1]++;
        }
    }
    s_di_valid = true;
    s_st.di = di;
    state_unlock();
}

static void read_di(void)
{
    uint8_t oe = 0x00;
    uint8_t raw = 0;
    esp_err_t wr = i2c_master_write_to_device(IO_I2C_PORT, IO_CH422_OE, &oe, 1, IO_I2C_TICKS);
    esp_err_t rd = i2c_master_read_from_device(IO_I2C_PORT, IO_CH422_IN, &raw, 1, IO_I2C_TICKS);
    /* Restore immediately. No log, no delay, no other call between these two. */
    oe = 0x01;
    i2c_master_write_to_device(IO_I2C_PORT, IO_CH422_OE, &oe, 1, IO_I2C_TICKS);
    i2c_master_write_to_device(IO_I2C_PORT, IO_CH422_OUT, &s_io_out, 1, IO_I2C_TICKS);
    publish_di(raw, wr == ESP_OK && rd == ESP_OK);
}

static void set_do(uint8_t mask, uint8_t value)
{
    mask = (uint8_t)(mask & 0x03);
    s_do_bits = (uint8_t)((s_do_bits & (uint8_t)~mask) | (value & mask));
    /* Logical ON -> OC pin LOW. OC2/OC3 are unused and stay high. */
    ch422_write_do(IO_DO_ACTIVE_LOW ? (uint8_t)(0x0F & (uint8_t)~s_do_bits) : s_do_bits);
    state_lock();
    s_st.do_ = s_do_bits;
    state_unlock();
}

static int bcd_to_int(uint8_t v)
{
    return (int)((v >> 4) * 10 + (v & 0x0F));
}

static void read_rtc(void)
{
    uint8_t reg = 0x04;
    uint8_t b[7];
    memset(b, 0, sizeof b);
    char hms[12];
    char date[16];
    bool ok = false;
    esp_err_t err = i2c_master_write_read_device(IO_I2C_PORT, IO_RTC_ADDR, &reg, 1, b, sizeof b,
                                                 IO_I2C_TICKS);
    if (err == ESP_OK && (b[0] & 0x80) == 0) {
        int sec = bcd_to_int((uint8_t)(b[0] & 0x7F));
        int min = bcd_to_int((uint8_t)(b[1] & 0x7F));
        int hour = bcd_to_int((uint8_t)(b[2] & 0x3F));
        int day = bcd_to_int((uint8_t)(b[3] & 0x3F));
        int mon = bcd_to_int((uint8_t)(b[5] & 0x1F));
        int year = 2000 + bcd_to_int(b[6]);
        if (sec >= 0 && sec <= 59 && min >= 0 && min <= 59 && hour >= 0 && hour <= 23 &&
            day >= 1 && day <= 31 && mon >= 1 && mon <= 12 && year >= 2000 && year <= 2099) {
            char hms_tmp[16];
            char date_tmp[16];
            snprintf(hms_tmp, sizeof hms_tmp, "%02u:%02u:%02u", (unsigned)hour, (unsigned)min,
                     (unsigned)sec);
            snprintf(date_tmp, sizeof date_tmp, "%04u-%02u-%02u", (unsigned)year, (unsigned)mon,
                     (unsigned)day);
            memcpy(hms, hms_tmp, sizeof hms);
            memcpy(date, date_tmp, sizeof date);
            ok = true;
        }
    }
    if (!ok) {
        snprintf(hms, sizeof hms, "--");
        snprintf(date, sizeof date, "--");
    }
    state_lock();
    s_st.rtc_ok = ok;
    memcpy(s_st.rtc_hms, hms, sizeof s_st.rtc_hms);
    memcpy(s_st.rtc_date, date, sizeof s_st.rtc_date);
    state_unlock();
}

static bool i2c_probe(uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return false;
    }
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)((addr << 1) | I2C_MASTER_WRITE), true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(IO_I2C_PORT, cmd, IO_SCAN_TICKS);
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

static twai_timing_config_t timing_for(uint32_t bitrate)
{
    switch (bitrate) {
    case 125000: {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_125KBITS();
        return t;
    }
    case 250000: {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
        return t;
    }
    case 500000: {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
        return t;
    }
    case 1000000: {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_1MBITS();
        return t;
    }
    case 50000:
    default: {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_50KBITS();
        return t;
    }
    }
}

static void can_push(const twai_message_t *m)
{
    io_can_frame_t fr;
    memset(&fr, 0, sizeof fr);
    fr.id = m->identifier;
    fr.ext = m->extd != 0;
    fr.dlc = m->data_length_code > 8 ? 8 : m->data_length_code;
    memcpy(fr.data, m->data, fr.dlc);
    fr.timestamp_ms = (uint32_t)now_ms();
    state_lock();
    memmove(&s_st.can_rx[1], &s_st.can_rx[0], sizeof(s_st.can_rx[0]) * (IO_RX_RING - 1));
    s_st.can_rx[0] = fr;
    if (s_st.can_rx_n < IO_RX_RING) {
        s_st.can_rx_n++;
    }
    s_st.can_rx_count++;
    s_st.can_rx_seq++;
    state_unlock();
}

static void can_apply_status(void)
{
    twai_status_info_t info;
    if (twai_get_status_info(&info) != ESP_OK) {
        return;
    }
    io_can_state_t st = IO_CAN_RUNNING;
    if (info.state == TWAI_STATE_BUS_OFF) {
        st = IO_CAN_BUS_OFF;
        if (!s_recover_sent) {
            twai_initiate_recovery();
            s_recover_sent = true;
        }
    } else if (info.state == TWAI_STATE_RECOVERING) {
        st = IO_CAN_RECOVERING;
        s_recover_sent = false;
    } else if (info.state == TWAI_STATE_STOPPED) {
        st = IO_CAN_STOPPED;
        s_recover_sent = false;
    } else if (info.tx_error_counter >= 128 || info.rx_error_counter >= 128) {
        st = IO_CAN_ERR_PASSIVE;
        s_recover_sent = false;
    } else {
        s_recover_sent = false;
    }
    state_lock();
    s_st.can_state = st;
    s_st.can_tec = info.tx_error_counter;
    s_st.can_rec = info.rx_error_counter;
    s_st.can_err_count = info.bus_error_count;
    state_unlock();
}

static void can_stop(void)
{
    if (s_can_up) {
        twai_stop();
        twai_driver_uninstall();
        s_can_up = false;
    }
    s_recover_sent = false;
    state_lock();
    s_st.can_state = IO_CAN_STOPPED;
    s_st.can_tec = 0;
    s_st.can_rec = 0;
    state_unlock();
}

static void can_start(uint32_t bitrate, bool listen)
{
    if (bitrate != 50000 && bitrate != 125000 && bitrate != 250000 && bitrate != 500000 &&
        bitrate != 1000000) {
        bitrate = 50000;
    }
    if (s_can_up) {
        can_stop();
    }
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
        IO_CAN_TX_GPIO, IO_CAN_RX_GPIO, listen ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL);
    twai_timing_config_t t = timing_for(bitrate);
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    esp_err_t err = twai_driver_install(&g, &t, &f);
    if (err == ESP_OK) {
        err = twai_start();
        if (err != ESP_OK) {
            twai_driver_uninstall();
        }
    }
    if (err != ESP_OK) {
        s_can_up = false;
        ESP_LOGW(TAG, "CAN start failed");
        state_lock();
        s_st.can_state = IO_CAN_STOPPED;
        s_st.can_err_count++;
        state_unlock();
        return;
    }
    s_can_up = true;
    s_can_listen = listen;
    s_can_bitrate = bitrate;
    s_recover_sent = false;
    state_lock();
    s_st.can_state = IO_CAN_RUNNING;
    s_st.can_bitrate = bitrate;
    state_unlock();
}

static void can_send(const io_cmd_t *cmd)
{
    if (!s_can_up || s_can_listen) {
        state_lock();
        s_st.can_err_count++;
        state_unlock();
        return;
    }
    twai_message_t m;
    memset(&m, 0, sizeof m);
    m.identifier = cmd->id;
    m.data_length_code = cmd->dlc > 8 ? 8 : cmd->dlc;
    memcpy(m.data, cmd->data, m.data_length_code);
    if (twai_transmit(&m, pdMS_TO_TICKS(20)) == ESP_OK) {
        state_lock();
        s_st.can_tx_count++;
        state_unlock();
    } else {
        state_lock();
        s_st.can_err_count++;
        state_unlock();
    }
}

static void can_poll(void)
{
    if (!s_can_up) {
        return;
    }
    twai_message_t m;
    while (twai_receive(&m, 0) == ESP_OK) {
        can_push(&m);
    }
    can_apply_status();
}

static void line_format(const uint8_t *bytes, int n, bool is_tx)
{
    io_line_t ln;
    memset(&ln, 0, sizeof ln);
    ln.is_tx = is_tx;
    int ti = 0;
    int hi = 0;
    for (int i = 0; i < n; i++) {
        uint8_t c = bytes[i];
        if (c == '\r' || c == '\n') {
            continue;
        }
        if (ti < IO_LINE_TEXT - 1) {
            ln.text[ti++] = (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
        }
        if (hi < IO_LINE_HEX - 4) {
            int wrote = snprintf(ln.hex + hi, (size_t)(IO_LINE_HEX - hi), hi ? " %02X" : "%02X",
                                 (unsigned)c);
            if (wrote > 0) {
                hi += wrote;
            }
        }
    }
    ln.text[ti] = '\0';
    state_lock();
    if (s_st.rs485_line_n < IO_LINE_RING) {
        s_st.rs485_line[s_st.rs485_line_n] = ln;
        s_st.rs485_line_n++;
    } else {
        memmove(&s_st.rs485_line[0], &s_st.rs485_line[1],
                sizeof(s_st.rs485_line[0]) * (IO_LINE_RING - 1));
        s_st.rs485_line[IO_LINE_RING - 1] = ln;
    }
    s_st.rs485_log_seq++;
    if (is_tx) {
        s_st.rs485_tx_seq++;
    } else {
        s_st.rs485_rx_seq++;
    }
    state_unlock();
}

static void line_flush(void)
{
    if (s_asm_n <= 0) {
        return;
    }
    line_format(s_asm, s_asm_n, false);
    s_asm_n = 0;
}

static void rs485_install(void)
{
    uart_config_t cfg = {
        .baud_rate = IO_RS_BAUD_DEFAULT,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(IO_RS_UART, IO_RS_RX_BUF, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(IO_RS_UART, &cfg);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(IO_RS_UART, IO_RS_TX_GPIO, IO_RS_RX_GPIO, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "UART2 install failed");
    }
    state_lock();
    s_st.rs485_baud = IO_RS_BAUD_DEFAULT;
    state_unlock();
}

static void rs485_config(uint32_t baud)
{
    if (baud < 1200 || baud > 1000000) {
        return;
    }
    if (uart_set_baudrate(IO_RS_UART, baud) == ESP_OK) {
        state_lock();
        s_st.rs485_baud = baud;
        state_unlock();
    }
}

static void rs485_send(const uint8_t *bytes, uint8_t len)
{
    if (len == 0) {
        return;
    }
    int n = uart_write_bytes(IO_RS_UART, bytes, len);
    if (n < 0) {
        return;
    }
    state_lock();
    s_st.rs485_tx_bytes += (uint32_t)n;
    state_unlock();
    line_format(bytes, n, true);
}

static void rs485_poll(void)
{
    uint8_t buf[64];
    int n = uart_read_bytes(IO_RS_UART, buf, sizeof buf, 0);
    int64_t us = esp_timer_get_time();
    if (n > 0) {
        state_lock();
        s_st.rs485_rx_bytes += (uint32_t)n;
        state_unlock();
        for (int i = 0; i < n; i++) {
            uint8_t c = buf[i];
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                line_flush();
                continue;
            }
            if (s_asm_n < IO_LINE_MAX) {
                s_asm[s_asm_n++] = c;
            }
            s_asm_last_us = us;
            if (s_asm_n >= IO_LINE_MAX) {
                line_flush();
            }
        }
    } else if (s_asm_n > 0 && (us - s_asm_last_us) >= IO_LINE_IDLE_US) {
        line_flush();
    }
}

static void maybe_di(void)
{
#if IO_DI_POLL_ENABLE
    int64_t ms = now_ms();
    if (ms >= s_next_di) {
        s_next_di = ms + IO_DI_POLL_MS;
        read_di();
    }
#else
    (void)s_next_di;
#endif
}

static void run_scan(void);

static void handle_cmd(const io_cmd_t *cmd)
{
    switch (cmd->kind) {
    case IO_CMD_SET_DO:
        set_do(cmd->mask, cmd->value);
        break;
    case IO_CMD_READ_DI:
        read_di();
        break;
    case IO_CMD_I2C_SCAN:
        run_scan();
        break;
    case IO_CMD_CAN_START:
        can_start(cmd->bitrate, cmd->listen_only);
        break;
    case IO_CMD_CAN_STOP:
        can_stop();
        break;
    case IO_CMD_CAN_SEND:
        can_send(cmd);
        break;
    case IO_CMD_RS485_CONFIG:
        rs485_config(cmd->baud);
        break;
    case IO_CMD_RS485_SEND:
        rs485_send(cmd->bytes, cmd->len > 64 ? 64 : cmd->len);
        break;
    default:
        break;
    }
}

static void drain_cmds(void)
{
    io_cmd_t cmd;
    while (xQueueReceive(s_q, &cmd, 0) == pdTRUE) {
        if (cmd.kind == IO_CMD_I2C_SCAN && s_scanning) {
            continue;
        }
        handle_cmd(&cmd);
    }
}

static void run_scan(void)
{
    if (s_scanning) {
        return;
    }
    s_scanning = true;
    const int total = IO_SCAN_LAST - IO_SCAN_FIRST + 1;
    state_lock();
    s_st.scan_busy = true;
    s_st.scan_progress = 0;
    found_clear_all();
    state_unlock();
    int done = 0;
    for (int addr = IO_SCAN_FIRST; addr <= IO_SCAN_LAST; addr++) {
        bool hit = i2c_probe((uint8_t)addr);
        done++;
        state_lock();
        if (hit) {
            found_set(addr);
        }
        s_st.scan_progress = (uint8_t)((done * 100) / total);
        state_unlock();
        drain_cmds();
        maybe_di();
        if (now_ms() >= s_next_rtc) {
            s_next_rtc = now_ms() + IO_RTC_PERIOD_MS;
            read_rtc();
        }
        can_poll();
        rs485_poll();
        vTaskDelay(1);
    }
    state_lock();
    s_st.scan_busy = false;
    s_st.scan_progress = 100;
    s_st.scan_seq++;
    state_unlock();
    s_scanning = false;
}

static void io_task(void *arg)
{
    (void)arg;
    /* Touch reset leaves 0x2E. The port never calls bl_on(), so put 0x1E back
     * before anything else touches the expander. */
    ch422_restore();
    /* Drive both DO outputs OFF before anything else (OC pins high). */
    ch422_write_do(IO_DO_ACTIVE_LOW ? 0x0F : 0x00);
    rs485_install();
    s_next_di = 0;
    s_next_rtc = 0;
    for (;;) {
        io_cmd_t cmd;
        if (xQueueReceive(s_q, &cmd, pdMS_TO_TICKS(20)) == pdTRUE) {
            handle_cmd(&cmd);
        }
        maybe_di();
        if (now_ms() >= s_next_rtc) {
            s_next_rtc = now_ms() + IO_RTC_PERIOD_MS;
            read_rtc();
        }
        can_poll();
        rs485_poll();
    }
}

void bashcat_io_start(void)
{
    if (s_q != NULL) {
        return;
    }
    memset(&s_st, 0, sizeof s_st);
    memcpy(s_st.rtc_hms, "--", 3);
    memcpy(s_st.rtc_date, "--", 3);
    s_st.can_state = IO_CAN_STOPPED;
    s_st.can_bitrate = s_can_bitrate;
    s_st.rs485_baud = IO_RS_BAUD_DEFAULT;
    s_mu = xSemaphoreCreateMutex();
    s_q = xQueueCreate(IO_QUEUE_LEN, sizeof(io_cmd_t));
    if (s_mu == NULL || s_q == NULL) {
        ESP_LOGE(TAG, "io queue alloc failed");
        return;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(io_task, "bashcat_io", IO_TASK_STACK, NULL, IO_TASK_PRIO,
                                             NULL, IO_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "io task create failed");
    }
}

bool bashcat_io_post(const io_cmd_t *cmd)
{
    if (cmd == NULL || s_q == NULL) {
        return false;
    }
    return xQueueSend(s_q, cmd, 0) == pdTRUE;
}

void bashcat_io_snapshot(io_state_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_mu == NULL) {
        memset(out, 0, sizeof *out);
        memcpy(out->rtc_hms, "--", 3);
        memcpy(out->rtc_date, "--", 3);
        out->rs485_baud = IO_RS_BAUD_DEFAULT;
        out->can_bitrate = 50000;
        return;
    }
    state_lock();
    *out = s_st;
    state_unlock();
}
