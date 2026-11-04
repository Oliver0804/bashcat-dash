# BASHCAT Dash — v2 規格：交互過場

v1 已在實機（ESP32-S3-Touch-LCD-4.3B）跑起來，數字正常跳動。v2 目標：**更多的交互與過場動畫**，同時引導使用者到 bashcat.net。
規則同 v1（SPEC.md）：配色、無霓虹/陰影/漸層、英文字、不改 `lvgl_port.c` / `waveshare_rgb_lcd_port.c` / `lvgl_port.h`，觸控已開啟（GT911）。

## 1. 多頁結構（3 頁）
用自建容器 + 自己的滑動動畫或 `lv_tileview`（已啟用）都可，**不要用 lv_scr_load_anim 每次重建畫面**。
- Page 0 `DASH`：現有儀表頁
- Page 1 `SYSTEM`：系統資訊頁，扁平 key/value 列表 + 小 bar：chip model/rev（`esp_chip_info`）、cores、CPU MHz、flash size（`esp_flash_get_size`）、PSRAM size、MAC（`esp_read_mac`）、IDF version（`esp_get_idf_version()`）、LVGL version、reset reason（`esp_reset_reason`）、uptime、min free heap（`esp_get_minimum_free_heap_size`）
- Page 2 `ABOUT`：`BASHCAT` 大字標 + tagline `把 idea 變成可以動的東西` 的英文版 `Turning ideas into things that move.` + **QR code 指向 `https://bashcat.net`**（啟用 `CONFIG_LV_USE_QRCODE=y`，`lv_qrcode_create`，深色模組 `#E3E7EA` 於 `#0B0D0F` 不好掃——QR 請用 **淺底深碼**：底 `#E3E7EA`、碼 `#0B0D0F`，外圍 12px 淺色 quiet zone 圓角卡片）+ 文字 `bashcat.net` + `github.com/Oliver0804/bashcat-dash`

## 2. 導覽與過場
- **左右滑動手勢**（`LV_EVENT_GESTURE` + `lv_indev_get_gesture_dir`）切頁；同時頂部 bar 中央放 3 個頁面指示器（小圓點/短橫），當前頁是綠色長條，切頁時**寬度與位置有動畫**（像 iOS page control）
- 頂部 bar 的頁面名稱 `01 · dash` / `02 · system` / `03 · about` 切換時做 **垂直滾動式文字轉場**（舊的往上淡出、新的從下淡入）
- 切頁滑動 280–350ms ease_out
- **進入頁面的 stagger**：每次進入 SYSTEM 頁，列表每一列依序從右 16px 滑入 + 淡入（每列延遲 40ms）；進入 ABOUT 頁，QR 卡片從 0.9 縮放到 1（可用 `transform_zoom` 或改尺寸）+ 淡入，文字 stagger
- 回到 DASH 頁時，三個儀表做一次**小幅 re-sweep**（從 0 掃到當前值，500ms），不要整段開機 sweep

## 3. 元件互動回饋
- 所有可點元件**按下回饋**：tile 按下時邊框變綠 `#34C77B`、背景微亮 `#151A1E`，放開恢復（用 style transition，`lv_style_transition_dsc_t`，120ms）
- **點 tile → 詳情展開**：tile 點擊後，一個詳情 overlay 面板從該 tile 位置展開（位置與尺寸動畫到中央 520×300 面板），裡面顯示該指標的大數字 + 最近 60 點的 lv_chart 歷史（HEAP、PSRAM、TEMP、FPS 各自要有 history buffer）+ min/max/avg；點 overlay 任意處或右上 `×` 收合回 tile 原位（反向動畫）。背景用 `#0B0D0F` opa 70% 的遮罩淡入（只做一次全屏淡入，可接受）
- 主儀表點擊切換指標（v1 已有）升級：弧線先**收回到 0**（200ms ease_in）再**掃到新值**（400ms ease_out），caption 做垂直滾動文字轉場
- 側邊兩個小儀表點擊：與主儀表**交換指標**（例如點 SRAM 小表，主表改顯示 SRAM），有同樣的收回/掃出動畫

## 4. 待機與喚醒
- 60 秒無觸控 → **待機畫面**：所有元件淡出，只剩中央大時鐘式 uptime `T+01:23:45`（montserrat 48）與一條緩慢呼吸的綠線、底部小字 `bashcat.net`；背光不動
- 觸控任意處喚醒：待機元素淡出 → 儀表 re-sweep 進場
- 可調參數 `UI_IDLE_TIMEOUT_MS` 預設 60000

## 5. 效能與正確性
- 全螢幕動畫只允許：切頁滑動、overlay 遮罩、待機切換。其他動畫物件要小
- 不要每次切頁 create/delete 大量物件；頁面建好一次，切換只改位置/隱藏
- 動畫 ready_cb 中刪除物件要小心（v1 有 lv_anim_del 的考量）；快速連點/連滑不可壞（動畫進行中再觸發要先 `lv_anim_del` 舊動畫或忽略）
- sample timer 在任何頁面都要持續更新 history buffer

## 6. 模擬器
更新 `sim/` 讓它能驗收 v2：在 sim/main.c 加入一段腳本化事件序列（press/release 座標與時間），並在關鍵時間點輸出 PNG：
- 開機完成後 t≈5000 截圖；左滑到 SYSTEM 過程中與完成後；再左滑到 ABOUT（QR 可見）；右滑兩次回 DASH（看 re-sweep）；點 HEAP tile 展開 overlay 過程中/完成後；點 overlay 收合；點左側小儀表交換；模擬 61 秒無操作進入待機並截圖；點擊喚醒並截圖
- 需要手勢就在 indev 送出移動軌跡（多個 10ms 步進的座標），讓 LVGL 自己偵測 gesture
- 截圖檔名帶時間與事件名，例如 `sim/out/v2_07400ms_swipe_left_mid.png`；並重新產生 `sim/out/v2.gif`（400 寬）

## 7. 交付
- `idf.py build` 零錯誤（環境指令同 v1），模擬器 `bash sim/build.sh` 可跑
- 更新 `main/ui/NOTES.md`：新增的所有可調參數（巨集名稱、預設值、意義）整理成表格
- 完成回覆 DONE + 檔案清單 + 截圖清單
