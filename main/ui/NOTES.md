# BASHCAT Dash UI

LVGL 8.4 儀表畫面。`bashcat_ui_start()` 在 `app_main` 持有 `lvgl_port_lock()` 時呼叫一次。之後的計時器與動畫都在 LVGL task 裡跑，不從別的 task 碰 LVGL。

字型只有 Montserrat。分隔點用 U+2022 `•`（字型有收）。U+00B7 `·` 不在這套字裡，畫出來會是缺字框。度數符號 `°`（U+00B0）有收。

觸控已開啟（GT911）。七頁順序是 dash、io、i2c、can、rs485、system、about。左右滑一次換鄰頁，點頂部頁點可以直接跳到那一頁。點主儀表循環 CPU → SRAM free → PSRAM used → chip temp。點側邊小表會與主表交換指標。點底部 tile 展開該指標的詳情。

## 檔案

| 檔案 | 角色 |
| --- | --- |
| `main/ui/bashcat_ui.h` | `bashcat_ui_start()` |
| `main/ui/bashcat_ui.c` | 畫面、動畫、取樣、七頁軌道 |
| `main/ui/bashcat_ui_v3.h` | 頁碼、v3 頁的 build / enter / apply |
| `main/ui/bashcat_ui_v3.c` | io、i2c、can、rs485 四頁 |
| `main/main.c` | 面板起來後先 `bashcat_io_start()`，再在 LVGL lock 裡呼叫 `bashcat_ui_start()` |
| `main/CMakeLists.txt` | 編進 `ui/bashcat_ui.c`、`ui/bashcat_ui_v3.c`、`io/bashcat_io.c` |
| `sdkconfig.defaults` | runtime stats、Montserrat 48、關掉 LVGL perf overlay、LVGL task stack 8 KB |

`lvgl_port.c` 與 `waveshare_rgb_lcd_port.c` 沒有改。

## 畫面

800×480。上排高 48：`BASHCAT`、綠點 + `ONLINE`、右側 `T+hh:mm:ss`，下一行右側是座標與 `est. 2018`。中央 300 px、270° 主儀表（CPU）。左右 164 px 副儀表（SRAM free、PSRAM used）。底部 sparkline 60 點，加上 HEAP / PSRAM / CHIP TEMP / FPS 四個 tile。

主儀表刻度 51 格、每 5 格一條主刻度。副儀表 21 格、每 4 格一條主刻度。弧起點在 7:30（rotation 135），掃 270°，缺口在下方。每顆表有一條 `#1E2328` 軌道弧，上面疊數值弧。v3.1 起刻度、刻度數字與軌道弧烘焙成一張 PSRAM snapshot，數值弧改成 `lv_arc`，只重畫變動的扇形。

## 資料

| 讀數 | 來源 | 函式 |
| --- | --- | --- |
| CPU load | 兩核 `ulTaskGetIdleRunTimeCounterForCore()`，ESP timer 微秒，對 `dt * 核數` | `sample_cpu()` |
| SRAM free | `heap_caps_get_free_size/total_size(MALLOC_CAP_INTERNAL)` | `sample_mem()` |
| PSRAM used / free | 同上，`MALLOC_CAP_SPIRAM`。副儀表是 used %，tile 是 free MB | `sample_mem()` |
| Chip temp | `temperature_sensor_*`，量程 -10..80 °C（S3 誤差最小的一檔） | `sensors_init()` / `sample_temp()` |
| FPS | `monitor_cb` 的重畫次數/秒（500 ms 視窗）。單位行是 1 秒內最大的 `time_ms` | `fps_monitor_cb()` / `sample_fps()` / `fps_ms_show()` |
| Uptime | `esp_timer_get_time()` | `update_uptime()` |

取樣週期 `UI_SAMPLE_MS`（500）。第一次呼叫 `sample_cpu()` 只建立基準，不產出百分比。

## 動畫在哪個函式

| 演出 | 函式 | 曲線 |
| --- | --- | --- |
| 開機字標淡入並上移（720 ms） | `intro_in_exec()`，由 `start_intro()` 啟動 | ease out |
| 綠線從中間向兩側展開（860 ms，延遲 80 ms） | `intro_line_exec()`，`intro_in_ready()` 接上 | ease in-out |
| 字標淡出上移，綠線一起淡出（680 ms，延遲 160 ms） | `intro_out_exec()`，`intro_line_ready()` 接上 | ease in |
| 進儀表、拆掉開機畫面 | `dash_reveal()` | — |
| 三顆表 0→100 | `sweep_up_exec()` / `start_sweep()` | ease in-out，900 ms |
| 100→各自的實際值 | `sweep_down_exec()` / `sweep_up_ready()` | ease in-out，800 ms |
| 自檢結束，進入常態 | `sweep_down_ready()` | — |
| tile 與 sparkline 面板依序淡入 | `stagger_fade()` / `fade_exec()` | ease out，每個延遲 80 ms |
| 狀態點呼吸，2 s 一個來回 | `start_breath()` / `breath_exec()` | ease in-out，無限 |
| 數值平滑（弧、數字、tile） | `gauge_exec()` → `gauge_show()`，`tile_exec()` | ease out，400 ms |
| CPU 弧色：≤80 綠、>80 琥珀、>95 紅 | `color_exec()` / `apply_color()` / `color_at()` | ease out，380 ms |
| 點主儀表換指標，或點側邊小表與主表交換 | `on_gauge_clicked()` → `start_retract_sweep()`。弧先收到 0，再掃到新值。caption 走 `roll_set()` | ease in 200 ms，接著 ease out 400 ms |
| 切頁滑動 | `go_page()` / `x_exec()`，軌道一次建好，只改 x | ease out，`UI_TRACK_MS` 300 ms |
| 頁點寬度與位置 | `dots_layout()` | ease out，320 ms |
| 頁名垂直滾動 | `roll_set()` | ease out，280 ms |
| 進入 SYSTEM，每列從右側 16 px 滑入 | `system_play()` | ease out，280 ms，每列延遲 40 ms |
| 進入 ABOUT，QR 卡片 0.9→1 並淡入，文字 stagger | `about_play()` | ease out，卡片 320 ms，文字 280 ms、延遲 40 ms |
| 回到 DASH 的小幅 re-sweep | `page_settled()` → `start_resweep()` | ease out，500 ms。開機自檢不走這條 |
| tile 按下：邊框轉綠、底色 `#151A1E` | `init_press_style()`，style transition | 120 ms |
| tile 詳情從原位展開到 520×300，遮罩淡入 | `overlay_open()` / `ov_exec()` | ease out，300 ms |
| 詳情收合 | `overlay_close()` | ease out，300 ms |
| 60 秒無觸控進待機，再點一下喚醒 | `enter_idle()` / `on_idle_press()` / `idle_gone()` | 淡入淡出 400 ms，喚醒後 re-sweep |

開機三個階段用不同的 exec callback 串 ready，同一時間只跑一段，加總 2500 ms。自檢的上升與回落各用一條動畫同時驅動三顆表，避免同一幀刪掉彼此的 ready。

顏色只作用在主儀表的數值弧和中央數字。副儀表與 sparkline 維持綠色。主指標不是 CPU 時，弧色回到綠色。

## 可調參數

都在 `bashcat_ui.c` 開頭的 `#define`：

| 名稱 | 預設 | 意思 |
| --- | --- | --- |
| `UI_SAMPLE_MS` | 500 | 取樣週期 |
| `UI_VALUE_ANIM_MS` | 400 | 數值過渡 |
| `UI_COLOR_ANIM_MS` | 380 | 閾值換色 |
| `UI_SWEEP_UP_MS` | 900 | 自檢掃到 100 |
| `UI_SWEEP_DOWN_MS` | 800 | 自檢落到實際值 |
| `UI_TILE_FADE_MS` | 280 | tile 淡入時間 |
| `UI_TILE_STAGGER_MS` | 80 | tile 交錯延遲 |
| `UI_BREATH_HALF_MS` | 1000 | 呼吸半週期（來回 2 s） |
| `UI_INTRO_IN_MS` | 720 | 字標進場 |
| `UI_INTRO_GAP1_MS` | 80 | 進場後到展開綠線 |
| `UI_INTRO_LINE_MS` | 860 | 綠線展開 |
| `UI_INTRO_GAP2_MS` | 160 | 綠線後到退場 |
| `UI_INTRO_OUT_MS` | 680 | 退場 |
| `UI_CPU_WARN` | 80 | 超過變琥珀 |
| `UI_CPU_CRIT` | 95 | 超過變紅 |
| `UI_CHART_POINTS` | 60 | sparkline 點數（500 ms 一點，約 30 s） |
| `UI_LINE_W` | 420 | 開機綠線最終寬度 |

版面在 `build_dashboard()`。頂部列高 `UI_HEADER_H`（52），下面是 800×428 的視窗，裡面一條 5600 寬的軌道一次放好七頁。主儀表在 DASH 頁 (250, 4)、300 px，螢幕座標 (250, 56)。副儀表頁內 y=72、164 px，螢幕 y=124。底列頁內 y=312，螢幕 y=364。HEAP tile 中心約在螢幕 (410, 416)，左側小表中心約在 (98, 206)。

數字用固定寬、置中的 label，更新走 `lv_label_set_text_static()` 配靜態 buffer，取樣回呼裡不另外 malloc。

## v2

三頁只建一次，不重載畫面。第 0 頁 DASH 是上面的儀表。第 1 頁 SYSTEM 是扁平列表：CHIP、CORES、CPU、FLASH、PSRAM、MAC、IDF、LVGL、RESET、UPTIME、MIN HEAP。MIN HEAP 那列底下有一條 4 px 的綠條。第 2 頁 ABOUT 是大字 `BASHCAT`、tagline `Turning ideas into things that move.`、淺底深碼的 QR（`https://bashcat.net`）、`bashcat.net`、`github.com/Oliver0804/bashcat-dash`。

手勢掛在畫面的 `LV_EVENT_GESTURE`。偵測到之後呼叫 `lv_indev_wait_release()`，避免滑動結尾又變成點擊。頁面滑動進行中可以再滑；點儀表或 tile 要等滑動停。詳情打開時不切頁、不進待機。

取樣計時器在每一頁都跑，HEAP / PSRAM / TEMP / FPS 各自累積 60 點。CPU 火花線仍只記 CPU。顏色只在主儀表現在顯示 CPU 時才會變琥珀或紅；CPU 換到側邊小表時，小表維持綠色。

待機看 `lv_disp_get_inactive_time(NULL)`。開機自檢、頁面滑動、詳情展開期間不進待機。進待機只淡一層全螢幕遮罩，背光不動。時鐘、頂部列、SYSTEM 的 UPTIME 共用同一條 `T+hh:mm:ss`。

主儀表刻度標籤的 label gap 是主表 20、小表 6。弧線 `r_mod` 兩邊都是 `-(major_len + 2)`。小表刻度數字透明度為 0，只留刻度線。這組刻度畫進 snapshot，不再每幀重算。

### v2 可調參數

都在 `bashcat_ui.c` 開頭，接在 v1 那組 `#define` 後面。

| 名稱 | 預設 | 意思 |
| --- | --- | --- |
| `UI_PAGE_MS` | 320 | 頁點與 ABOUT QR 縮放 |
| `UI_TRACK_MS` | 300 | 切頁軌道滑動 |
| `UI_PAGE_STAGGER_MS` | 40 | SYSTEM 列與 ABOUT 文字的交錯延遲 |
| `UI_PAGE_SLIDE_PX` | 16 | 進場時從右側偏移的距離 |
| `UI_ENTER_MS` | 280 | 列與文字滑入的時間 |
| `UI_RESWEEP_MS` | 500 | 回到 DASH 或喚醒後，三顆表從 0 掃到目前值 |
| `UI_RETRACT_MS` | 200 | 換指標時弧先收到 0 |
| `UI_SWEEP_TO_MS` | 400 | 收回之後掃到新值 |
| `UI_PRESS_MS` | 120 | 按下回饋的 style transition |
| `UI_OVERLAY_MS` | 300 | 詳情展開與收合 |
| `UI_IDLE_TIMEOUT_MS` | 60000 | 無觸控多久進待機 |
| `UI_IDLE_FADE_MS` | 400 | 待機層淡入、淡出 |
| `UI_IDLE_LINE_HALF_MS` | 1600 | 待機綠線呼吸的半週期 |
| `UI_ROLL_MS` | 280 | 頁名與 caption 的垂直滾動 |
| `UI_ABOUT_ZOOM_START` | 230 | ABOUT 卡片進場縮放。LVGL 的 256 是 1.0，230 約 0.9 |
| `UI_QR_SIZE` | 168 | QR 模組邊長 |
| `UI_QR_QUIET` | 12 | 淺色 quiet zone |
| `UI_QR_CARD` | 192 | 卡片邊長，`UI_QR_SIZE + UI_QR_QUIET * 2` |
| `UI_DOT_OFF_W` | 6 | 非當前頁的點寬 |
| `UI_DOT_ON_W` | 18 | 當前頁的綠條寬 |
| `UI_DOT_H` | 6 | 頁點高度 |
| `UI_DOT_GAP` | 6 | 頁點間距 |
| `UI_HEADER_H` | 52 | 頂部列高度，視窗從這裡開始 |
| `UI_MASK_OPA` | 178 | 詳情遮罩不透明度，約 70% |
| `HEX_PRESS` | `0x151A1E` | 按下時的底色 |
| `UI_IO_POLL_MS` | 100 | 向後端要 `io_state_t` 的週期 |
| `UI_SYS_PITCH` | 30 | SYSTEM 每一列的高度 |

v2 當時是三頁，SYSTEM 在第 1 頁、ABOUT 在第 2 頁。v3 把這兩頁改到第 5、第 6 頁，列距從 36 改成 30。

## v3

頁序與標題（分隔點都是 U+2022）：

| 頁 | 標題 |
| --- | --- |
| 0 | `01  •  dash` |
| 1 | `02  •  io` |
| 2 | `03  •  i2c` |
| 3 | `04  •  can` |
| 4 | `05  •  rs485` |
| 5 | `06  •  system` |
| 6 | `07  •  about` |

頁點可以點。`go_page()` 一次滑到目標頁，不逐頁播放。滑動進行中、詳情打開、待機中，點頁點不會跳。IO / I2C / CAN / RS485 進場走 `bashcat_ui_v3_enter()`，每個區塊從右側 16 px 滑入，延遲 40 ms。回歸座標用 `lv_obj_get_style_x()`。建頁時這四頁還在視窗外，`lv_obj_get_x()` 讀到的 coords 仍是 0，會把右欄整組拉回左邊。

SYSTEM 在 UPTIME 後面多了 RTC 與 PERIPH，MIN HEAP 仍是最後一列。列從 y=4 開始，列高 30。MIN HEAP 的數字貼在列頂，綠條在 y=21、高 3，避免跟 16 px 字重疊。RTC 顯示 `hh:mm:ss` 或 `--`。PERIPH 形如 `I2C ok  •  CAN running  •  RS485 115200`。這兩列由 100 ms 的 `io_timer_cb()` 更新，字串沒變就不重畫。

四個新頁的 widget 只呼叫 `bashcat_io_post()` / 吃 snapshot，不碰 driver。DO 在後端確認前顯示 `...`。I2C 的 0x20–0x3F 在清單收成一列 `0x20-3F  CH422G`，格子仍各別點亮。

### v3 可調參數

在 `bashcat_ui_v3.c` 開頭，另外 `UI_IO_POLL_MS` 與 `UI_SYS_PITCH` 在 `bashcat_ui.c`。

| 名稱 | 預設 | 意思 |
| --- | --- | --- |
| `UI_IO_POLL_MS` | 100 | snapshot 週期 |
| `UI_PULSE_MS` | 400 | DI 變化時的脈衝環 |
| `UI_SCAN_STAGGER_MS` | 30 | 掃到的位址依序彈出 |
| `UI_LAMP_MS` | 80 | RS485 RX/TX 燈閃一下 |
| `UI_POP_MS` | 220 | I2C 格子從 205 縮放到 256 |
| `UI_SEND_MS` | 400 | CAN SEND 底下的進度線 |
| `UI_NUM_MS` | 300 | 計數數字滾動 |
| `UI_SEG_MS` | 320 | 速率選擇器的滑塊 |
| `UI_ROW_MS` | 240 | CAN / RS485 新列滑入 |
| `UI_DI_POINTS` | 100 | DI 波形點數 |
| `UI_SYS_PITCH` | 30 | SYSTEM 列距 |

## v3.1 效能

實機 FPS 偏低。direct mode 已經只刷髒區。時間花在 `lv_meter` 每次改弧都重畫刻度與遮罩弧，以及整物件 `opa` 讓之後每次重畫都走 layer。

`sdkconfig` 與 `sdkconfig.defaults` 都要改。已有 `sdkconfig` 時 defaults 不會重套。bootloader 維持 `-Os`。

- `CONFIG_COMPILER_OPTIMIZATION_PERF=y`，應用程式 `-O2`
- `CONFIG_LV_DISP_DEF_REFR_PERIOD=15`。面板 VSYNC 約 39 Hz（16 MHz / (816×496)）。模擬器 `sim/lv_conf.h` 的 refresh 維持 10，改了會讓動畫相位對不上截圖
- instruction cache 32 KB、data cache 64 KB。data cache line 維持 64 B
- `CONFIG_EXAMPLE_LVGL_PORT_TASK_PRIORITY` 維持 2
- `CONFIG_LV_USE_SNAPSHOT=y`
- PSRAM 維持 80 MHz。`CONFIG_SPIRAM_SPEED_120M` 不開。這是可選實驗項：Octal PSRAM 120 MHz，頻寬約 +50%，需要 `CONFIG_IDF_EXPERIMENTAL_FEATURES=y`（已開）。要實機確認穩定再打開

儀表的刻度、刻度數字、灰色軌道用 `lv_snapshot_take_to_buf()` 畫一次，格式 `LV_IMG_CF_TRUE_COLOR_ALPHA`，緩衝區 `heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`。主表約 270 KB，兩顆小表約 161 KB。這塊記憶體跟著 UI 活著，不要對它呼叫 `lv_snapshot_free()`。數值弧改 `lv_arc`，`lv_arc_set_value` 只重畫變動的扇形。變色只改主表弧的 `LV_PART_INDICATOR` 與中央數字。門檻仍是 CPU >80 琥珀、>95 紅，沒有遲滯。label gap 主表 20、小表 6，小表刻度數字透明度 0，都在這張靜態圖裡。caption 的 `LV_ALIGN_BOTTOM_MID` 要再減 meter 的 pad（主表 12、小表 8）：對齊對象從有 pad 的 meter 換成沒有 pad 的容器，不減的話標題會往下掉一截 pad。

FPS tile 的大數字仍是 500 ms 視窗的重畫次數/秒。單位行從 `fps` 改成 `N ms`，是 `monitor_cb` 的 `time_ms` 在 1 秒內的最大值。每 2 秒 `ESP_LOGI("perf", ...)` 印 fps、平均/最大 render ms、平均 flush 像素。畫面靜止時 tile 的 fps 低，因為 `monitor_cb` 只在真的重畫時計數。`ms` 用來看出畫一幀要多久。

切頁軌道是 `UI_TRACK_MS` 300。頁點與 ABOUT QR 仍是 `UI_PAGE_MS` 320。滑動中照樣取樣、照樣記歷史，不啟動儀表、tile、顏色動畫，也不把新點推進 chart。

整物件 `style opa` 只留在進場動畫。結束後設回 `LV_OPA_COVER`，並 `lv_obj_remove_local_style_prop(obj, LV_STYLE_OPA, 0)`。淡出到 0 的改成隱藏，再移掉 local opa。狀態點與待機綠線仍是 `bg_opa`。

模擬器 DASH 視窗 `[3000, 13000)`，步進 10 ms，1000 步，其中 946 步有 flush。平均取「有 flush 的幀」：

| | 有 flush 的幀平均 | 含未 flush 步的平均 | px 合計 |
| --- | --- | --- | --- |
| 優化前 | 15,033 px | 14,221 px | 14,221,437 |
| 優化後 | 12,596 px | 11,916 px | 11,916,743 |

規格裡的「約 14 萬 px」是整顆表被 invalidate 的估算。模擬器量到的優化前平均是 15,033 px：`lv_meter` 的弧更新本來就只 invalidate 楔形，整表 invalidate 出現在 CPU 跨過 80% 的變色。快取之後不再每幀重算刻度與遮罩，flush 像素從 15,033 降到 12,596。`-O2`、15 ms refresh、加大 cache 要在實機才看得到。模擬器編譯本來就是 `-O2`。

截圖對照：非 DASH 頁差 0 px。DASH 在 t=5000 與 t=12000 各差 386 px。其中 166 px 是 FPS 單位從 `fps` 改成 `0 ms`，其餘是 snapshot 抗鋸齒合成與直接繪製差一個色階。刻度、數字、弧、caption 的位置一致。
