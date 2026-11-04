#pragma once

#include <stdbool.h>
#include <stdint.h>

/* UI talks to peripherals only through this header. No driver types. */

#define IO_RX_RING   8
#define IO_LINE_RING 8
#define IO_LINE_TEXT 48
#define IO_LINE_HEX  100

typedef enum {
    IO_CMD_SET_DO = 1,
    IO_CMD_READ_DI,
    IO_CMD_I2C_SCAN,
    IO_CMD_CAN_START,
    IO_CMD_CAN_STOP,
    IO_CMD_CAN_SEND,
    IO_CMD_RS485_CONFIG,
    IO_CMD_RS485_SEND,
} io_cmd_kind_t;

typedef enum {
    IO_CAN_STOPPED = 0,
    IO_CAN_RUNNING,
    IO_CAN_ERR_PASSIVE,
    IO_CAN_BUS_OFF,
    IO_CAN_RECOVERING,
} io_can_state_t;

typedef struct {
    io_cmd_kind_t kind;
    uint8_t mask;          /* SET_DO: bits to change, bit0 = DO0, bit1 = DO1 */
    uint8_t value;         /* SET_DO: new bit values within mask */
    uint32_t bitrate;      /* CAN_START */
    bool listen_only;      /* CAN_START */
    uint32_t id;           /* CAN_SEND */
    uint8_t dlc;           /* CAN_SEND, 0..8 */
    uint8_t data[8];       /* CAN_SEND */
    uint32_t baud;         /* RS485_CONFIG */
    uint8_t len;           /* RS485_SEND, 0..64 */
    uint8_t bytes[64];     /* RS485_SEND */
} io_cmd_t;

typedef struct {
    uint32_t id;
    bool ext;
    uint8_t dlc;
    uint8_t data[8];
    uint32_t timestamp_ms;
} io_can_frame_t;

typedef struct {
    char text[IO_LINE_TEXT];
    char hex[IO_LINE_HEX];
    bool is_tx;
} io_line_t;

typedef struct {
    uint8_t di; /* bit0 = DI0 (CH422G IO0), bit1 = DI1 (CH422G IO5) */
    uint8_t do_;
    uint32_t di_change_count[2];

    bool scan_busy;
    uint8_t scan_progress; /* 0..100 */
    uint8_t found[16];     /* bit (addr) of 0..127 */
    uint32_t scan_seq;     /* increments when a scan finishes */
    bool rtc_ok;
    char rtc_hms[12];      /* "hh:mm:ss" or "--" */
    char rtc_date[16];     /* "yyyy-mm-dd" or "--" */

    io_can_state_t can_state;
    uint32_t can_bitrate;
    uint32_t can_rx_count;
    uint32_t can_tx_count;
    uint32_t can_err_count;
    uint32_t can_tec;
    uint32_t can_rec;
    io_can_frame_t can_rx[IO_RX_RING]; /* [0] is the newest */
    uint8_t can_rx_n;
    uint32_t can_rx_seq;

    uint32_t rs485_baud;
    uint32_t rs485_rx_bytes;
    uint32_t rs485_tx_bytes;
    io_line_t rs485_line[IO_LINE_RING]; /* [0] oldest, last is newest */
    uint8_t rs485_line_n;
    uint32_t rs485_rx_seq;
    uint32_t rs485_tx_seq;
    uint32_t rs485_log_seq;
} io_state_t;

static inline bool io_found_get(const io_state_t *st, unsigned addr)
{
    if (st == 0 || addr >= 128u) {
        return false;
    }
    return (st->found[addr >> 3] & (uint8_t)(1u << (addr & 7u))) != 0;
}

void bashcat_io_start(void);
bool bashcat_io_post(const io_cmd_t *cmd);
void bashcat_io_snapshot(io_state_t *out);
