# BASHCAT Dash — v3 規格：週邊互動（GPIO / I2C / CAN / RS485）

在 v2（三頁 + 過場）之上，新增「後端」週邊驅動與對應的互動頁面。視覺規則、禁止修改的檔案同 SPEC.md / SPEC_v2.md。

## 0. 硬體事實（取自 Waveshare 官方範例，已核對）

| 週邊 | 介面 | 腳位 / 位址 |
| --- | --- | --- |
| I2C 主匯流排 | legacy `driver/i2c.h`，port 0，400 kHz（`waveshare_rgb_lcd_port.c` 的 `i2c_master_init()` 已安裝，**不可重複安裝、不可改用新版 i2c_master driver**） | SDA 8 / SCL 9 |
| 隔離 DI0 / DI1 | CH422G IO0 (`0x01`) / IO5 (`0x20`) | 讀：寫 `0x24 ← 0x00`（IO 輸入模式）後讀 `0x26` |
| 隔離 DO0 / DO1 | CH422G OC0 (`0x01`) / OC1 (`0x02`) | 寫 `0x23 ← bits` |
| CH422G 其他 IO | IO1 觸控重置、IO2 背光、IO3 LCD 重置、IO4 SD CS | 輸出值寫 `0x38`，背光開時官方寫 `0x1E` |
| CAN | TWAI，`driver/twai.h` | TX GPIO15 / RX GPIO16（官方預設 50 kbit/s） |
| RS485 | UART2，自動方向（無 RTS） | TX GPIO44 / RX GPIO43，預設 115200 8N1 |
| RTC | PCF85063 | I2C `0x51` |
| 觸控 | GT911 | I2C `0x5D` 或 `0x14` |

### ⚠ CH422G 陷阱（必須處理）
CH422G 的 IO0–IO7 方向是**整組切換**。讀 DI 要把整組切成輸入（`0x24 ← 0x00`），此時背光 IO2、觸控重置 IO1、LCD 重置 IO3 也會變成輸入。官方 `CH422G_od_output()` 也寫了 `0x24 ← 0x00`。規則：
1. 維護一個 shadow `s_io_out = 0x1E`（背光開、各重置拉高、SD CS 高）。
2. **每次** DI 讀取或 DO 寫入後，立刻 `0x24 ← 0x01`（IO_OE）並 `0x38 ← s_io_out` 恢復，兩次 I2C 之間不得插入延遲以外的動作。
3. DO 寫入順序：`0x24 ← 0x01` → `0x23 ← do_bits` → `0x38 ← s_io_out`。
4. DI 讀取順序：`0x24 ← 0x00` → `0x26` 讀 1 byte → `0x24 ← 0x01` → `0x38 ← s_io_out`。
5. DI 輪詢週期 `IO_DI_POLL_MS`（預設 100）。提供 `IO_DI_POLL_ENABLE` 巨集（預設 1），實機若背光閃爍可關掉、改成頁面上手動「READ」。
6. 所有 CH422G 存取都在同一個 backend task 內，序列化；legacy I2C driver 每筆交易內部有鎖，與觸控讀取共用匯流排是安全的。

## 1. 後端模組 `main/io/`

新增 `main/io/bashcat_io.c/.h`（可拆檔）。**UI 不得直接呼叫任何驅動 API。**

```c
void bashcat_io_start(void);                 // app_main 在 LCD init 之後呼叫，建立 io task（core 0，prio 5，stack 6 KB）
bool bashcat_io_post(const io_cmd_t *cmd);   // UI → backend，xQueueSend 0 tick，不阻塞
void bashcat_io_snapshot(io_state_t *out);   // UI timer 每 100 ms 取一次，內部 mutex 複製
```

- `io_cmd_t` 種類：`SET_DO(mask,value)`、`READ_DI`、`I2C_SCAN`、`CAN_START(bitrate, listen_only)`、`CAN_STOP`、`CAN_SEND(id, dlc, data[8])`、`RS485_CONFIG(baud)`、`RS485_SEND(bytes,len≤64)`
- `io_state_t`：
  - `di` (2 bits)、`do_` (2 bits)、`di_change_count[2]`
  - I2C：`scan_busy`、`scan_progress` (0–100)、`found[128]` bitmap、`scan_seq`（每次掃描完成 +1）、RTC 時間（每秒讀 PCF85063 一次，BCD 解碼 `hh:mm:ss`、`yyyy-mm-dd`；讀失敗顯示 `--`）
  - CAN：`state`（STOPPED/RUNNING/ERR_PASSIVE/BUS_OFF/RECOVERING）、`bitrate`、`rx_count`、`tx_count`、`err_count`、`tx_err_counter`、`rx_err_counter`、最近 8 筆 RX ring（id、ext、dlc、data、時間戳）、`rx_seq`；BUS_OFF 自動 `twai_initiate_recovery()`
  - RS485：`baud`、`rx_bytes`、`tx_bytes`、最近 8 行 RX ring（以 `\n` 或 32 bytes 或 50 ms 閒置斷行；不可列印字元顯示成 `.`，同時保留 hex）、`rx_seq`、`tx_seq`
- I2C 掃描：0x08–0x77，每個位址 `start / addr|W / stop`，timeout 10 ms；掃描過程逐步更新 `scan_progress` 讓 UI 有動畫。已知名稱表：`0x51 RTC PCF85063`、`0x5D/0x14 TOUCH GT911`、`0x20–0x3F 區段標示 CH422G`（CH422G 會回應多個「功能位址」，UI 歸成一組）。
- CAN 預設不啟動（避免沒接匯流排時累積錯誤），由 UI 按 START；bitrate 選項 125k / 250k / 500k / 1M（並保留官方的 50k）。CAN_SEND 預設測試幀 ID `0x123`、資料為遞增計數。
- RS485 開機就啟動 UART2 115200；注意 GPIO43/44 在 ROM 開機時會吐 UART0 開機訊息到匯流排，寫進 NOTES。

## 2. 新頁面（加入 v2 的頁面系統，總共 7 頁）

順序：`01 dash` · `02 io` · `03 i2c` · `04 can` · `05 rs485` · `06 system` · `07 about`
頁面指示器改成 7 格；**點指示器可直接跳頁**（跳多頁時用一次滑動動畫，不要逐頁播）。

### 02 · io
- 左半 `INPUTS`：DI0、DI1 兩顆大型「指示燈」（直徑 72 圓，關：`#1E2328` 邊框空心；開：`#34C77B` 實心），狀態改變時有 **脈衝環**動畫（一個圓環從燈外擴散並淡出 400 ms）＋旁邊 `changes: N` 計數滾動
- 右半 `OUTPUTS`：DO0、DO1 兩個大型扁平開關（自繪或 `lv_switch` 重新上色：軌道 `#1E2328`、開啟 `#34C77B`），切換時 knob 滑動 + 下方 `HIGH/LOW` 文字垂直滾動轉場；按下開關後送 `SET_DO`，**以 backend snapshot 回讀為準**（命令送出但未確認時顯示 `…` 狀態）
- 底部一條 DI 時序圖：兩條數位波形（lv_chart 或自繪 step line），持續往左捲動 10 秒
- 頂部小字：`CH422G · DI 0x01/0x20 · DO OC0/OC1`

### 03 · i2c
- 仿 `i2cdetect` 的 8×16 位址格（列 0x00–0x70，欄 0–F），每格顯示兩位 hex，muted 色
- `SCAN` 按鈕：按下後一條綠色掃描游標依 `scan_progress` 逐格走過網格（格子短暫高亮），找到的位址**依序彈出**（stagger 30 ms、縮放 0.8→1 + 變綠底深字）
- 右側清單：找到的裝置 + 已知名稱（例 `0x51  RTC · PCF85063`），列表 stagger 滑入
- 右下 RTC 卡：大字 `hh:mm:ss`（秒數變化時數字垂直滾動）＋日期

### 04 · can
- 上方控制列：bitrate segmented 選擇器（選中段落的綠色底塊**滑動**到新位置）、`LISTEN` 開關、`START/STOP` 按鈕（按下有按壓回饋，狀態切換時文字轉場）
- 狀態燈 + 文字（`RUNNING` 綠 / `ERR PASSIVE` 琥珀 / `BUS OFF` 紅），顏色過渡
- 計數 tile：RX / TX / ERR / TEC / REC（數字平滑滾動）
- 中央 RX 幀列表（最近 8 筆）：每筆 `ID · DLC · DATA hex`，**新幀從頂部滑入**，舊的往下推，最底一筆淡出；新幀那一列背景短暫亮綠再退回
- `SEND 0x123` 按鈕：送出時按鈕上有一條進度細線劃過；TX tile 跳動
- 小 sparkline：每秒幀數

### 05 · rs485
- 上方：baud segmented（9600 / 19200 / 57600 / 115200）滑塊動畫、TX/RX 兩顆活動燈（有資料就閃 80 ms）
- 中央「終端」面板：最近 8 行 RX（`#111417` 底、行首 `>` muted），新行從底部推入（舊行上移）
- 下方快速發送按鈕：`PING`（送 `BASHCAT PING n\r\n`）、`HELLO`（送 `hello from bashcat.net\r\n`）、`0x55 AA`（送 hex `55 AA`）；TX 行以綠色 `<` 顯示在終端內
- 計數：RX bytes / TX bytes

### 06 · system（v2 已有）
加一列 `RTC` 顯示 PCF85063 時間；加一列 `PERIPH` 顯示 `I2C ok · CAN off · RS485 115200`。

## 3. 互動與動畫通則
- 所有按鈕：按下 120 ms style transition（邊框轉綠、背景 `#151A1E`），放開回復；被 disable 時 opa 40%
- segmented 選擇器、開關、狀態文字都要有轉場，不可瞬間跳變
- 頁面進入時內容 stagger（沿用 v2 機制）
- 待機（v2）時 backend 照常運作；待機中收到 CAN/RS485 新資料不要喚醒

## 4. 模擬器
- `sim/` 提供 `bashcat_io` 的 **stub 實作**（不編譯真實的 `main/io/*.c`）：DI 每 1.5 s 交替變化、SET_DO 立即反映、I2C 掃描 1.2 s 完成並找到 `0x14 0x24 0x38 0x51` 等、CAN START 後每 300 ms 產生一筆假幀、RS485 每 900 ms 收到一行假資料
- 腳本化事件：依序跳到 02–05 頁並各自操作（切 DO、按 SCAN、START CAN + SEND、按 PING），每頁至少 2 張截圖（動畫中 / 完成後），檔名 `sim/out/v3_<ms>_<page>_<event>.png`，並產生 `sim/out/v3.gif`

## 5. 交付
- `idf.py build` 零錯誤；`bash sim/build.sh` 可跑
- `main/io/NOTES.md`：後端架構、命令/狀態結構、CH422G 陷阱處理、每個可調巨集（名稱、預設值、意義）
- 回覆 DONE + 檔案清單 + 截圖清單
