# 硬體參數 · Waveshare ESP32-S3-Touch-LCD-4.3B

本頁整理 BASHCAT Dash 韌體實際使用的硬體設定，來源為 Waveshare 官方 ESP-IDF 範例包（`08_lvgl_Porting`）並經實機驗證。

> 官方文件：<https://docs.waveshare.net/ESP32-S3-Touch-LCD-4.3B/ESP-IDF>
> 官方範例包：<https://files.waveshare.net/wiki/ESP32-S3-Touch-LCD-4.3B/ESP32-S3-Touch-LCD-4.3B-BOX-Demo.zip>

## 晶片與記憶體

| 項目 | 值 |
| --- | --- |
| SoC | ESP32-S3（QFN56，實測 rev v0.2） |
| Flash | 16 MB，QIO，80 MHz |
| PSRAM | 8 MB Octal（OPI），80 MHz |
| CPU | 240 MHz 雙核 |
| USB | 原生 USB-Serial-JTAG（燒錄與 console 同一條 Type-C） |
| ESP-IDF | **v5.5.2**（官方要求；v6.0 未驗證） |
| LVGL | 8.4.0 |

## RGB LCD（800×480，RGB565，16-bit 並列）

| 訊號 | GPIO | | 訊號 | GPIO |
| --- | --- | --- | --- | --- |
| VSYNC | 3 | | DATA0 (B0) | 14 |
| HSYNC | 46 | | DATA1 | 38 |
| DE | 5 | | DATA2 | 18 |
| PCLK | 7 | | DATA3 | 17 |
| DISP | 未接（-1） | | DATA4 | 10 |
| RST | 未接（-1） | | DATA5 (G0) | 39 |
| | | | DATA6 | **0**（同時是 BOOT 綁定腳）|
| | | | DATA7 | 45 |
| | | | DATA8 | 48 |
| | | | DATA9 | 47 |
| | | | DATA10 | 21 |
| | | | DATA11 (R0) | 1 |
| | | | DATA12 | 2 |
| | | | DATA13 | 42 |
| | | | DATA14 | 41 |
| | | | DATA15 | 40 |

時序（800×480）：

| 參數 | 值 |
| --- | --- |
| Pixel clock | 16 MHz（1024×600 版為 21 MHz） |
| HSYNC pulse / back / front porch | 4 / 8 / 8 |
| VSYNC pulse / back / front porch | 4 / 8 / 8 |
| PCLK 極性 | `pclk_active_neg = 1` |
| Framebuffer | 位於 PSRAM，2 張（防撕裂 mode 3：雙緩衝 + LVGL direct mode）|
| Bounce buffer | 800 × 10 行（SRAM） |

## I2C 匯流排（觸控 / IO 擴充 / RTC）

| 項目 | 值 |
| --- | --- |
| SDA / SCL | GPIO 8 / GPIO 9 |
| 速率 | 400 kHz，I2C port 0 |
| 觸控 | GT911（`esp_lcd_touch_gt911`），透過 CH422G 重置 |
| IO 擴充 | CH422G：寫 `0x24` 設定模式、寫 `0x38` 設定輸出 |
| RTC | PCF85063 |
| 觸控重置用 GPIO | GPIO 4（輸出） |

### 背光（經 CH422G）

```c
// 進輸出模式
uint8_t v = 0x01; i2c_master_write_to_device(0, 0x24, &v, 1, timeout);
// 背光開：0x1E；背光關：0x1A
v = 0x1E;         i2c_master_write_to_device(0, 0x38, &v, 1, timeout);
```

### 觸控重置序列

`0x24 ← 0x01` → `0x38 ← 0x2C` → 延遲 100 ms → GPIO4 = 0 → 延遲 100 ms → `0x38 ← 0x2E` → 延遲 200 ms。

> 原廠範例把 `CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_GT911` 預設為 **0（觸控關閉）**。本韌體改為 1，並把 GT911 初始化失敗改成警告而非 abort，避免重開機迴圈。

## 隔離 DI / DO（依原廠電路圖）

電路圖：<https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3B/ESP32-S3-Touch-LCD-4.3B-Sch.pdf>（第 1 頁 DI/DO 區塊）

| 通道 | CH422G 腳 | 電路 | 極性 |
| --- | --- | --- | --- |
| DO0 / DO1 | OD0 / OD1（寫 `0x23`） | `3V3 → 510Ω → PC817 LED → ODx`；光耦輸出（集極接 5V）經 1kΩ 推 NPN（T2/T4），**集極 = DOUT，射極 = 板子 GND** | **active-low**：OD 腳拉 LOW 才導通 |
| DI0 / DI1 | EXIO0 / EXIO5（讀 `0x26`） | 4.7kΩ 上拉到 3V3，PC814 雙向光耦電晶體接 GND；輸入側 `DI_COM` 內部經 220Ω + 1N5822 接 5V | **active-low**：有輸入時讀到 0 |

韌體在 `main/io/bashcat_io.c` 用 `IO_DO_ACTIVE_LOW` / `IO_DI_ACTIVE_LOW` 轉成邏輯值，UI 上的 `ON` 就是輸出導通；開機時先把兩個 DO 設為 OFF。

### DO 是開集極「吸入」輸出 — 需要電源經過負載才看得到變化

DO 只能把 DOUT 拉到 GND，不會輸出電壓。什麼都不接直接量 DOUT 對 GND，ON/OFF 都量不到變化是正常的。

| 方式 | 接法 | 現象 |
| --- | --- | --- |
| 外部電源 + 負載 | `V+ (5–36V) → 負載（LED+1kΩ／繼電器）→ DOUTx`，`V− → 板子 GND` | ON：負載動作、DOUT ≈ 0V；OFF：DOUT ≈ V+ |
| 借板子 5V | `5V → 1kΩ → DOUTx` | ON ≈ 0V、OFF ≈ 5V |
| 迴路自測（官方 IO_Test） | `DOUT0 → DI0`、`DOUT1 → DI1` | DO0 ON 時 DI0 亮（DI_COM 內部已有 5V） |

- 每通道最大吸入 450 mA，5–36 V
- DO 輸出級的射極接**板子 GND**（只有控制訊號經光耦隔離），外部電源負極需共地
- 驅動電感性負載（繼電器、電磁閥）請自行加飛輪二極體

## CAN / RS485

| 項目 | 值 |
| --- | --- |
| CAN | TWAI，TX GPIO15 / RX GPIO16，韌體可選 50k / 125k / 250k / 500k / 1M，可切 listen-only |
| RS485 | UART2，TX GPIO44 / RX GPIO43，自動收發方向，可選 9600 / 19200 / 57600 / 115200 |
| 終端電阻 | 板上撥動開關，預設**關閉** |

GPIO43/44 也是 UART0 預設腳，ROM 開機時會把開機訊息送上 RS485 匯流排。

## LVGL port 設定（`sdkconfig.defaults`）

| 參數 | 值 | 說明 |
| --- | --- | --- |
| `CONFIG_EXAMPLE_LVGL_PORT_TASK_CORE` | 1 | LVGL task 綁 core 1 |
| `CONFIG_EXAMPLE_LVGL_PORT_TASK_PRIORITY` | 2 | |
| `CONFIG_EXAMPLE_LVGL_PORT_TASK_STACK_SIZE_KB` | 8 | lv_meter 遮罩需要較大堆疊 |
| `CONFIG_EXAMPLE_LVGL_PORT_TICK` | 2 ms | |
| `CONFIG_EXAMPLE_LVGL_PORT_AVOID_TEAR_MODE` | 3 | 雙緩衝 + direct mode |
| `CONFIG_LV_DISP_DEF_REFR_PERIOD` | 30 ms | |
| `CONFIG_FREERTOS_HZ` | 1000 | |
| `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` | y | CPU 負載量測 |
| `CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER` | y | 微秒計數 |
| `CONFIG_LV_FONT_MONTSERRAT_12/16/20/24/48` | y | UI 字型 |

## 已知狀況

- **燒錄後停在 `waiting for download`**：透過 USB 軟體重置（RTS/DTR）時，晶片可能每次都進下載模式（`boot:0x0 (DOWNLOAD)`）。燒錄本身成功；**按一下板上 RST 鍵或重新插拔 USB** 即可正常開機。GPIO0 同時是 LCD DATA6 與 BOOT 綁定腳。
- Montserrat 內建字型沒有中文與 U+00B7 `·`，畫面上用 U+2022 `•` 當分隔點。
