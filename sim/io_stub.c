/*
 * Host stand-in for main/io/bashcat_io.c. The simulator does not compile the
 * real driver file. Time comes from esp_timer, which the sim advances, so the
 * scripted screenshots are deterministic.
 *
 * DI alternates every 1.5 s. An I2C scan finishes in 1.2 s and reports
 * 0x14, 0x24, 0x38, 0x51. After CAN START a frame arrives every 300 ms.
 * An RS485 line arrives every 900 ms. SET_DO shows up on the next snapshot.
 */

#include "bashcat_io.h"

#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

#define STUB_DI_MS     1500
#define STUB_SCAN_MS   1200
#define STUB_CAN_MS    300
#define STUB_RS_MS     900

static io_state_t s_st;
static bool s_inited;
static uint8_t s_do;
static bool s_scan_on;
static int64_t s_scan_t0;
static bool s_can_on;
static bool s_listen;
static int64_t s_can_next;
static uint32_t s_can_n;
static int64_t s_rs_next;
static uint32_t s_rs_n;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void found_set(unsigned addr)
{
    if (addr >= 128u) {
        return;
    }
    s_st.found[addr >> 3] = (uint8_t)(s_st.found[addr >> 3] | (uint8_t)(1u << (addr & 7u)));
}

static void push_line(const char *text, const char *hex, bool is_tx, uint32_t bytes)
{
    io_line_t ln;
    memset(&ln, 0, sizeof ln);
    ln.is_tx = is_tx;
    snprintf(ln.text, sizeof ln.text, "%s", text);
    snprintf(ln.hex, sizeof ln.hex, "%s", hex);
    if (s_st.rs485_line_n < IO_LINE_RING) {
        s_st.rs485_line[s_st.rs485_line_n++] = ln;
    } else {
        memmove(&s_st.rs485_line[0], &s_st.rs485_line[1],
                sizeof(s_st.rs485_line[0]) * (IO_LINE_RING - 1));
        s_st.rs485_line[IO_LINE_RING - 1] = ln;
    }
    s_st.rs485_log_seq++;
    if (is_tx) {
        s_st.rs485_tx_seq++;
        s_st.rs485_tx_bytes += bytes;
    } else {
        s_st.rs485_rx_seq++;
        s_st.rs485_rx_bytes += bytes;
    }
}

static void push_frame(int64_t ms)
{
    s_can_n++;
    io_can_frame_t fr;
    memset(&fr, 0, sizeof fr);
    fr.id = 0x100u + (s_can_n & 0x7Fu);
    fr.dlc = 4;
    fr.data[0] = (uint8_t)(s_can_n & 0xFFu);
    fr.data[1] = (uint8_t)((s_can_n >> 8) & 0xFFu);
    fr.data[2] = 0x55;
    fr.data[3] = 0xAA;
    fr.timestamp_ms = (uint32_t)ms;
    memmove(&s_st.can_rx[1], &s_st.can_rx[0], sizeof(s_st.can_rx[0]) * (IO_RX_RING - 1));
    s_st.can_rx[0] = fr;
    if (s_st.can_rx_n < IO_RX_RING) {
        s_st.can_rx_n++;
    }
    s_st.can_rx_count++;
    s_st.can_rx_seq++;
}

static void format_tx(const uint8_t *bytes, unsigned len)
{
    char text[IO_LINE_TEXT];
    char hex[IO_LINE_HEX];
    int ti = 0;
    int hi = 0;
    text[0] = '\0';
    hex[0] = '\0';
    for (unsigned i = 0; i < len; i++) {
        uint8_t c = bytes[i];
        if (c == '\r' || c == '\n') {
            continue;
        }
        if (ti < IO_LINE_TEXT - 1) {
            text[ti++] = (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
        }
        int wrote = snprintf(hex + hi, sizeof hex - (size_t)hi, hi ? " %02X" : "%02X", (unsigned)c);
        if (wrote > 0 && hi + wrote < IO_LINE_HEX) {
            hi += wrote;
        }
    }
    text[ti] = '\0';
    push_line(text, hex, true, len);
}

static void init_state(void)
{
    if (s_inited) {
        return;
    }
    memset(&s_st, 0, sizeof s_st);
    memcpy(s_st.rtc_hms, "--", 3);
    memcpy(s_st.rtc_date, "--", 3);
    s_st.can_bitrate = 50000;
    s_st.rs485_baud = 115200;
    s_rs_next = STUB_RS_MS;
    s_inited = true;
}

static void catch_up(void)
{
    init_state();
    int64_t ms = now_ms();
    if (ms < 0) {
        ms = 0;
    }
    int phase = (int)(ms / STUB_DI_MS);
    s_st.di = (phase & 1) ? 0x01 : 0x02;
    s_st.di_change_count[0] = (uint32_t)phase;
    s_st.di_change_count[1] = (uint32_t)phase;
    s_st.do_ = s_do;

    if (s_scan_on) {
        int64_t el = ms - s_scan_t0;
        if (el >= STUB_SCAN_MS) {
            s_scan_on = false;
            s_st.scan_busy = false;
            s_st.scan_progress = 100;
            memset(s_st.found, 0, sizeof s_st.found);
            found_set(0x14);
            found_set(0x24);
            found_set(0x38);
            found_set(0x51);
            s_st.scan_seq++;
        } else {
            s_st.scan_busy = true;
            s_st.scan_progress = (uint8_t)((el * 100) / STUB_SCAN_MS);
        }
    }

    int sec = (int)(ms / 1000);
    int hh = (sec / 3600) % 24;
    int mm = (sec / 60) % 60;
    int ss = sec % 60;
    snprintf(s_st.rtc_hms, sizeof s_st.rtc_hms, "%02d:%02d:%02d", hh, mm, ss);
    snprintf(s_st.rtc_date, sizeof s_st.rtc_date, "2026-11-04");
    s_st.rtc_ok = true;

    if (s_can_on) {
        s_st.can_state = IO_CAN_RUNNING;
        int guard = 0;
        while (s_can_next <= ms && guard < 32) {
            push_frame(s_can_next);
            s_can_next += STUB_CAN_MS;
            guard++;
        }
    } else {
        s_st.can_state = IO_CAN_STOPPED;
    }

    int rguard = 0;
    while (s_rs_next <= ms && rguard < 64) {
        char text[IO_LINE_TEXT];
        char hex[IO_LINE_HEX];
        s_rs_n++;
        snprintf(text, sizeof text, "bashcat rx %u", (unsigned)s_rs_n);
        snprintf(hex, sizeof hex, "%02X %02X %02X", (unsigned)(s_rs_n & 0xFFu), 0x0Du, 0x0Au);
        push_line(text, hex, false, (uint32_t)strlen(text) + 2u);
        s_rs_next += STUB_RS_MS;
        rguard++;
    }
}

void bashcat_io_start(void)
{
    init_state();
}

bool bashcat_io_post(const io_cmd_t *cmd)
{
    if (cmd == NULL) {
        return false;
    }
    catch_up();
    switch (cmd->kind) {
    case IO_CMD_SET_DO:
        s_do = (uint8_t)((s_do & (uint8_t)~(cmd->mask & 0x03)) | (cmd->value & cmd->mask & 0x03));
        s_st.do_ = s_do;
        break;
    case IO_CMD_READ_DI:
        break;
    case IO_CMD_I2C_SCAN:
        if (!s_scan_on) {
            s_scan_on = true;
            s_scan_t0 = now_ms();
            s_st.scan_busy = true;
            s_st.scan_progress = 0;
            memset(s_st.found, 0, sizeof s_st.found);
        }
        break;
    case IO_CMD_CAN_START:
        if (!s_can_on) {
            s_can_next = now_ms() + STUB_CAN_MS;
        }
        s_can_on = true;
        s_listen = cmd->listen_only;
        s_st.can_bitrate = cmd->bitrate ? cmd->bitrate : 50000;
        s_st.can_state = IO_CAN_RUNNING;
        break;
    case IO_CMD_CAN_STOP:
        s_can_on = false;
        s_listen = false;
        s_st.can_state = IO_CAN_STOPPED;
        break;
    case IO_CMD_CAN_SEND:
        if (s_can_on && !s_listen) {
            s_st.can_tx_count++;
        } else {
            s_st.can_err_count++;
        }
        break;
    case IO_CMD_RS485_CONFIG:
        if (cmd->baud >= 1200 && cmd->baud <= 1000000) {
            s_st.rs485_baud = cmd->baud;
        }
        break;
    case IO_CMD_RS485_SEND:
        if (cmd->len > 0) {
            unsigned n = cmd->len > 64 ? 64 : cmd->len;
            format_tx(cmd->bytes, n);
        }
        break;
    default:
        break;
    }
    return true;
}

void bashcat_io_snapshot(io_state_t *out)
{
    if (out == NULL) {
        return;
    }
    catch_up();
    *out = s_st;
}
