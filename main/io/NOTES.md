# BASHCAT IO 後端

UI 只透過 `bashcat_io.h` 跟週邊說話：`bashcat_io_post()` 丟命令，`bashcat_io_snapshot()` 複製狀態。不直接呼叫 I2C、TWAI、UART。

`bashcat_io_start()` 在 `app_main` 裡、LCD 初始化之後、拿 LVGL lock 之前呼叫。一條 task 在 core 0、優先權 5、stack 6144 bytes，把 CH422G、I2C 掃描、RTC、CAN、RS485 全部串起來。佇列深度 16，UI 用 0 tick 丟命令，滿了就丟棄，不阻塞 LVGL。

I2C 用已經由面板初始化裝好的 legacy driver（port 0，SDA 8，SCL 9，400 kHz）。這裡不再 `i2c_driver_install`，也不換成新的 `i2c_master` driver。

## 命令與狀態

`io_cmd_t.kind`：

| kind | 欄位 |
| --- | --- |
| `IO_CMD_SET_DO` | `mask`、`value`，bit0 = DO0，bit1 = DO1 |
| `IO_CMD_READ_DI` | 立刻讀一次 |
| `IO_CMD_I2C_SCAN` | 掃 0x08–0x77 |
| `IO_CMD_CAN_START` | `bitrate`、`listen_only` |
| `IO_CMD_CAN_STOP` | — |
| `IO_CMD_CAN_SEND` | `id`、`dlc`、`data[8]` |
| `IO_CMD_RS485_CONFIG` | `baud` |
| `IO_CMD_RS485_SEND` | `bytes`、`len`（最多 64） |

`io_state_t` 給 UI 每 100 ms 複製一份。`di` / `do_` 各 2 bits。`found[16]` 是 128 個位址的 bitmap。`can_rx[0]` 是最新一幀，最多 8 筆。`rs485_line[0]` 是最舊的一行，最後一筆是最新。`rtc_hms` 失敗時是 `--`。

CAN 狀態：STOPPED、RUNNING、ERR_PASSIVE、BUS_OFF、RECOVERING。ERR_PASSIVE 不是 TWAI 的狀態，是 RUNNING 且 TEC 或 REC ≥ 128。BUS_OFF 每個回合只呼叫一次 `twai_initiate_recovery()`。執行中改 bitrate 或 listen-only 會停掉、卸載、再裝、再啟動。listen-only 不送幀，送了算錯誤。開機不啟動 CAN。

RS485 開機就用 UART2、115200 8N1、TX GPIO44、RX GPIO43，沒有 DE 腳。一行在 `\n`、滿 32 bytes、或閒置 50 ms 時送進 ring。不可列印字元在 `text` 記成 `.`，`hex` 保留原值。

## CH422G

隔離 DI0/DI1 是 IO0（0x01）與 IO5（0x20）。讀法是 0x24 ← 0x00，再從 0x26 讀 1 byte。隔離 DO0/DO1 是 OC0（0x01）與 OC1（0x02），寫 0x23。其餘 IO 的輸出寫 0x38：IO1 觸控 reset、IO2 背光、IO3 LCD reset、IO4 SD CS。

`waveshare_esp32_s3_touch_reset()` 結束時把 0x38 寫成 0x2E（IO1、IO2、IO3、IO5）。`wavesahre_rgb_lcd_bl_on()` 會寫 0x24 ← 0x01 再 0x38 ← 0x1E，但初始化沒有呼叫它。所以 LCD 起來之後晶片停在 0x2E，不是 0x1E。io task 的第一個動作是把方向拉回輸出、再把 shadow 寫回去，中間不做別的事。shadow 初值 `IO_SHADOW_INIT` 是 0x1E（背光開、reset 高、SD CS 高）。v3 不改背光，shadow 維持 0x1E。以後要改背光，必須先更新 shadow 再寫 0x38，而且不能在這一組處於輸入模式時寫。

每次 DI 讀或 DO 寫之後立刻恢復：

1. DI：0x24 ← 0x00，讀 0x26，然後 0x24 ← 0x01，0x38 ← shadow。讀失敗也要恢復。
2. DO：0x24 ← 0x01，0x23 ← do bits，0x38 ← shadow。不要再多做一次恢復。

這兩筆 I2C 之間不打 log、不拿 mutex、不做其他工作。

DI 輪詢預設 100 ms。若實機背光閃，把 `IO_DI_POLL_ENABLE` 設成 0，改由 IO 頁的 READ 手動讀。I2C 掃描每個位址只做 address ACK，不寫 0x24 / 0x38。掃描途中只在滿 100 ms 時才讀一次 DI，並順便處理 CAN、RS485、RTC 與命令佇列，每次 `vTaskDelay(1)`，避免優先權 5 的 task 餓死 LVGL。

## GPIO43 / GPIO44

主控台是 USB-Serial-JTAG，UART0 不是主控台。ROM boot 仍會把 UART0 的開機訊息打到 GPIO43 / GPIO44，這兩支腳就是 RS485 的 RX / TX，所以開機當下匯流排上會看到一段 ROM 輸出。韌體起來之後 UART2 才接管這兩支腳。

## 可調參數

都在 `main/io/bashcat_io.c` 開頭。

| 名稱 | 預設 | 意思 |
| --- | --- | --- |
| `IO_I2C_PORT` | `I2C_NUM_0` | 與面板共用的 legacy I2C |
| `IO_I2C_TICKS` | 50 ms | CH422G 與 RTC 的交易逾時 |
| `IO_CH422_OE` | `0x24` | 方向暫存器。0x01 輸出，0x00 輸入 |
| `IO_CH422_OC` | `0x23` | 隔離 DO，OC0 / OC1 |
| `IO_CH422_OUT` | `0x38` | IO0–IO7 輸出 |
| `IO_CH422_IN` | `0x26` | IO 輸入 |
| `IO_SHADOW_INIT` | `0x1E` | 開機要寫回的輸出。IO1、IO2、IO3、IO4 |
| `IO_DI0_MASK` | `0x01` | DI0 = IO0 |
| `IO_DI1_MASK` | `0x20` | DI1 = IO5 |
| `IO_DI_POLL_MS` | 100 | DI 輪詢週期 |
| `IO_DI_POLL_ENABLE` | 1 | 0 就關掉輪詢，只留手動 READ |
| `IO_RTC_ADDR` | `0x51` | PCF85063 |
| `IO_RTC_PERIOD_MS` | 1000 | 讀 RTC 的週期 |
| `IO_SCAN_FIRST` | `0x08` | 掃描起點 |
| `IO_SCAN_LAST` | `0x77` | 掃描終點 |
| `IO_SCAN_TICKS` | 10 ms | 每個位址的 ACK 逾時 |
| `IO_CAN_TX_GPIO` | 15 | TWAI TX |
| `IO_CAN_RX_GPIO` | 16 | TWAI RX |
| `IO_RS_UART` | `UART_NUM_2` | RS485 |
| `IO_RS_TX_GPIO` | 44 | RS485 TX |
| `IO_RS_RX_GPIO` | 43 | RS485 RX |
| `IO_RS_BAUD_DEFAULT` | 115200 | 開機鮑率，8N1 |
| `IO_RS_RX_BUF` | 1024 | UART RX ring |
| `IO_LINE_MAX` | 32 | 一行最多幾個字元就送出 |
| `IO_LINE_IDLE_US` | 50000 | 超過這段沒新字元就送出半行 |
| `IO_QUEUE_LEN` | 16 | UI → 後端佇列 |
| `IO_TASK_STACK` | 6144 | bytes，不是 words |
| `IO_TASK_PRIO` | 5 | 高於 LVGL 的 2 |
| `IO_TASK_CORE` | 0 | LVGL 在 core 1 |

觸控 GT911 在 0x5D 或 0x14。模擬器不編這個檔，改走 `sim/io_stub.c`。
