# BASHCAT Dash — 韌體 UI 規格

板子：Waveshare ESP32-S3-Touch-LCD-4.3B，800×480 RGB565，LVGL **8.4.0**（`components/lvgl__lvgl`），ESP-IDF v5.5.2。
專案基底為 Waveshare `08_lvgl_Porting`，顯示/觸控 port 已可用，**不要改動** `lvgl_port.c`、`waveshare_rgb_lcd_port.c` 的時序與腳位。

## 任務
新增 `main/ui/bashcat_ui.c` + `main/ui/bashcat_ui.h`（可再拆 `main/ui/*.c`），提供：

```c
void bashcat_ui_start(void);   // 由 app_main 在 lvgl_port_lock() 內呼叫一次
```
並更新 `main/CMakeLists.txt` 的 SRCS / INCLUDE_DIRS，`main.c` 改成呼叫 `bashcat_ui_start()`（取代 lv_demo_*）。

## 視覺（bashcat.net 現行風格）
- 背景 `#0B0D0F`，面板 `#111417`，線條/格線 `#1E2328`
- 主文字 `#E3E7EA`，次要 `#99A2AA`，muted `#6B747C`
- 唯一強調色：克制綠 `#34C77B`；警告 `#F5A524`；危險 `#E5484D`
- **無霓虹、無發光、無陰影、無漸層、無掃描線**。扁平、細線、大量留白，圓角 12px，邊框 1px `#1E2328`
- 字型：只用已啟用的 Montserrat 12/16/20/24（若要大數字可在 sdkconfig.defaults 加開 `CONFIG_LV_FONT_MONTSERRAT_32/40/48=y`）。**畫面文字一律英文**（無中文字型）
- 品牌細節：頂部左側 `BASHCAT` 字標（letter spacing 寬），右側 `25.033°N · 121.565°E`、`est. 2018` 這類中點分隔的小字；區塊標題用 `01 · system`、`02 · power` 編號風格（小寫、muted 色）

## 儀表風格（汽車/航空儀表的感覺，但扁平）
版面（800×480）：
- 頂部 bar（高 ~44）：BASHCAT 字標 · 綠色狀態點 + `ONLINE` · 右側 uptime `T+00:12:34`
- 中央主儀表：大型 270° 弧形 gauge（直徑 ~300），刻度線（主刻度 + 次刻度，用 lv_meter 或自繪），指針或弧形進度，中心巨大數字 + 單位，代表 **CPU load %**
- 左右兩側：兩個較小的弧形副儀表（例如 **internal SRAM free %** 與 **PSRAM used %**），或線性條狀表
- 底部：一條即時 sparkline（lv_chart，綠色細線，60 點滾動）顯示 CPU load 歷史 + 3~4 個數值 tile（`HEAP`, `PSRAM`, `CHIP TEMP`, `FPS`）
- 數值字體 tabular 對齊感，單位小字 muted

## 資料來源（真實數據，不要只放假資料）
- CPU load：用 FreeRTOS idle hook 或 `esp_cpu_get_cycle_count` 估算；簡單作法：`ulTaskGetIdleRunTimeCounter()` 需要 `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y`，請在 sdkconfig.defaults 加上並實作。若太麻煩可用 idle task 計數器法。
- 記憶體：`heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` / `MALLOC_CAP_SPIRAM` 及 total
- 晶片溫度：`driver/temperature_sensor.h`（IDF 5.x API）
- FPS：LVGL 自身 refresh 計數（可在 lv_timer 中用 `lv_disp_get_inv_buf_size` 不行的話，就用 flush 次數/秒；或直接移除 `CONFIG_LV_USE_PERF_MONITOR` 並自己計）
- 數據更新：lv_timer 每 500ms 取樣；**不要在 LVGL task 以外呼叫 LVGL API**

## 動畫演出（必要）
1. **開機演出（~2.5s）**：黑畫面 → `BASHCAT` 字標淡入並由下往上位移 → 綠色細線從中央向兩側展開 → 字標淡出/上移
2. **儀表自檢 sweep**：所有 gauge 指針/弧從 0 掃到 100 再回落到實際值（像汽車點火自檢），ease-in-out，tile 依序 stagger 淡入（每個延遲 80ms）
3. **常態動畫**：數值變動用 `lv_anim` 平滑過渡（300–500ms，ease_out），不要跳格；數字也要滾動式過渡
4. **閾值狀態**：CPU > 80% 弧線變 `#F5A524`，> 95% 變 `#E5484D`；顏色切換要過渡
5. 頂部狀態點每 2s 呼吸一次（opa 動畫，細微）
6. 觸控（若可用）：點主儀表可切換主儀表顯示的指標（CPU → SRAM → PSRAM → TEMP），切換時數字與弧線有轉場動畫

## 效能約束
- 這塊板子 PSRAM framebuffer，**全畫面重繪很貴**。避免大面積半透明、避免每幀全螢幕 invalidate；動畫物件盡量小
- 不要使用 lv_obj 陰影 / 漸層 / blur
- 避免在 timer callback 做 malloc 洩漏；用 static 物件

## 交付與驗收
- `idf.py build` 必須零錯誤（環境：`source ~/.espressif/v5.5.2/esp-idf/export.sh`）
- 不要執行 flash（驗收方會燒錄）
- 完成後寫一份 `main/ui/NOTES.md`：檔案結構、每個動畫在哪個函式、可調參數
