# claude-desktop-buddy 參考分析（為 Claudi 而做）

> 分析日期：2026-06-10
> 分析對象：`/tmp/claude-desktop-buddy`（主）、`/tmp/robot-bridge`（輔）
> 目標項目：`/Users/philip/Development/claudi`（ESP32-C6 + 1.47" ST7789 LCD 寵物伴侶）
> 方法：用 3 個平行 subagent 分工分析（見下方「分析方法」），再由主 agent 綜合。**今次只做分析，冇改任何 Claudi code。**

---

## 0. 分析方法（我點樣做）

我用咗 `superpowers:dispatching-parallel-agents` skill，派咗 **3 個平行 subagent**，各自有獨立 context、唔互相干擾，分工如下：

| Subagent | 負責範疇 | 主要讀咩 |
|---|---|---|
| **A — Protocol / Transport** | BLE/serial/HTTP 協議、endpoint、xfer chunk 傳輸、host bridge | buddy `REFERENCE.md` / `ble_bridge.*` / `xfer.h` / `data.h` / `tools/*`；robot-bridge `ARCHITECTURE.md` / `websocket_handler.py` / `api.py` |
| **B — Firmware / Display / Asset pipeline** | 顯示渲染、動畫模型、gif→device asset pipeline、記憶體策略 | buddy `character.cpp` / `buddy.cpp` / `prep_character.py` / `manifest.json`；Claudi `slime_assets.*` / `display*.{h,cpp}` |
| **C — UX State Machine** | 寵物狀態機、Claude 活動→狀態映射、approval/waiting/running、transcript 顯示 | buddy `main.cpp`(state machine) / `stats.h` / `data.h`；robot-bridge 對話階段 FSM |

主 agent 自己亦讀咗 Claudi 嘅 `README.md`、`.claude/hooks/claudi_hook.py`、`SKILL.md`，掌握現狀後再綜合。

---

## 1. Repo 核心概念摘要

### 1.1 claude-desktop-buddy 係咩

**佢就係 Claudi 想做嘅嘢嘅官方版本。** 呢個係 Anthropic 官方文檔化嘅 maker 協議：**Claude 桌面 app（Mac/Windows，要開 Developer Mode）直接用 BLE 同硬件寵物對話**。硬件係 **M5StickC Plus（ESP32 classic + ST7789V2 135×240 LCD）**。

核心特性：
- 桌面 app 會 scan BLE，揀名以 `Claude` 開頭嘅裝置，OS 配對，之後背景自動重連。
- 桌面 app **唔係送生命週期事件**，而係送一個 **session 狀態快照（snapshot）**：`running`/`waiting`/`total` 計數、一行 `msg`、transcript `entries[]`、`tokens`、同埋一個 approval `prompt{id,tool,hint}`。
- 裝置自己由快照 **derive** 出寵物狀態（priority ladder），仲可以喺裝置上面 **approve/deny** 權限請求，再經 BLE 送返 `{"cmd":"permission","id":...,"decision":"once"|"deny"}` 畀桌面。
- 螢幕除咗寵物，仲畫 **transcript HUD、approval card、stats/clock 頁**。
- 動畫有兩套：18 隻內建 ASCII 角色（純 code），同埋 **GIF 角色包**（放喺 LittleFS，行時用 AnimatedGIF decode）。GIF 包可以經 USB（`uploadfs`）或者 BLE folder-push 上機，唔使重 flash。

### 1.2 robot-bridge 係咩（互補概念）

完全唔同性質：一個 host 端 **FastAPI WebSocket daemon**，行 XiaoZhi 語音協議 + 二進制 Opus/JPEG 幀 + MCP JSON-RPC 中繼去控制 actuator。對 Claudi 嘅價值係 **架構模式**，唔係照搬：
- 裝置 **主動 dial-in** host daemon（device 係 client），相對 buddy 係 host 連 device。
- **`/ota` config bootstrap**：裝置開機 HTTP GET `/ota` 攞返 WebSocket URL + 時間，解決硬編 IP 問題。
- **`/internal/*` fan-in**：任何本地 process 經一條連線推命令落裝置。
- **對話階段 FSM**：`idle→wake→listen→think→reply`，每個 phase 對應 LED 顏色——「決策喺上游、即時回饋喺薄薄嘅本地層」呢個原則好啱 Claudi。

### 1.3 Claudi 現狀（baseline）

- ESP32-C6 + ST7789V3 172×320，PlatformIO + Arduino（pioarduino）。
- 有乾淨嘅 `Display` 抽象層（可換 driver）。
- WiFi SoftAP + HTTP server，endpoint：`GET /status`、`GET|POST /render?title=&line1..3=`、`GET /pet/state?state=`、`POST /reboot`。
- Asset 係 **firmware-baked RGB565 C array**（`slime_assets.*`），每幀 172×172 ≈ **59 KB**，改圖要重 flash。
- Claude Code **hook**（`claudi_hook.py`）把每個生命週期事件 map 成單一寵物狀態，fire-and-forget GET 推去裝置。BLE 只係 advertise。
- 12 個狀態：`idle, blink, happy, sleepy, curious, alert, bored, working, thinking, attention, idea, excited`。

**一句總結差異**：buddy = 「host 算好快照、device 自己 derive 狀態、可雙向 approve」；Claudi = 「hook 把每個事件硬 map 成一個狀態、單向推、改圖要重 flash」。buddy 嘅模型明顯更豐富、更貼近 Claudi 想去嘅 companion 方向。

---

## 2. 對 Claudi 最值得參考嘅 7 點（已排序）

### ① 由「事件洪流」轉做「活動快照 + priority ladder」狀態機 〔P0，最重要〕
buddy 唔會見到 `PreToolUse(Bash)`，佢只見到「1 running, 1 waiting」。host 做晒聚合，device 用一條 **優先級階梯** derive：
```
waiting>0      → attention   （最高優先：等 approval）
recentlyDone   → celebrate   （一次性）
running>=N     → busy
else           → idle / 長時間無 → sleep
```
建議 Claudi 改成 **hybrid**：base state 由聚合活動（running/waiting 計數）決定，事件味道嘅狀態（`idea`/`curious`）就做 2–3 秒 one-shot overlay。咁寵物先唔會每個 tool call 都亂跳。
> 參考：buddy `src/main.cpp:479-490`（`derive()`）；wire shape `REFERENCE.md:45-72`。

### ② one-shot overlay（帶 timeout 自動回 base）機制 〔P0〕
`triggerOneShot(state, durMs)` 設 `oneShotUntil = now+durMs`，到期自動 revert 返 base。呢個係處理 transient 反應（`idea`/`excited`/`celebrate`/`heart`/`dizzy`）嘅正確原語，唔會卡死喺某個狀態。
> 參考：`src/main.cpp:487-490, 1002`。

### ③ approval / waiting / running 做 first-class，仲要 escalate 〔P0〕
buddy 嘅 `attention`（=`waiting>0` 或有 `prompt.id`）係最高優先：**強制 wake screen、keep 住唔熄、red LED 閃、跳去 approval card、過 ~10 秒 timer 變紅**。呢個正正係 Claudi 想要嘅 approval-waiting 體驗。
> 參考：`src/main.cpp:481, 1005-1039, 734-736`。

### ④ transcript / message 顯示（HUD + approval card）〔P1〕
buddy 把 host 送嘅 `entries[]`（近期 transcript，最新喺前）word-wrap 成 21 字一行、畫 3 行、最新光舊嘅暗、B 掣可向上 scroll；approval card 顯示 `tool` + `hint` + 已過秒數。呢個直接滿足 Claudi 嘅「transcript/message display」目標，而且 wire shape 已經有得抄。
> 參考：`drawHUD` `main.cpp:890-936`；`drawApproval` `main.cpp:725-768`。

### ⑤ GIF-on-LittleFS 動畫 + 可上傳 asset（取代 59KB/幀 raw array）〔P0〕
Claudi 而家每幀 raw RGB565 59KB 焗死喺 flash，改圖要重 flash。buddy 用 **indexed-palette + LZW 嘅標準 GIF 放喺 LittleFS partition**，行時 `bitbank2/AnimatedGIF` 逐 scanline decode，細 10–50 倍，仲可以 OTA 換圖唔使重 flash。
> 參考：`character.cpp:102-136, 374-400`；`platformio.ini`（LittleFS partition）。

### ⑥ `prep_character.py` asset pipeline（gif→device）〔P0，幾乎可照搬〕
最妙嘅一招係 **全狀態統一 bounding box**：對所有狀態所有幀做 `getbbox()` union，用同一個 crop box，令角色喺每個動畫都一樣大細（唔會 idle 大、busy 細）。然後 resize 到固定闊度、flatten 落 bg 色、quantize 到 64 色、重出多幀 GIF、改寫 manifest。Claudi 嘅多狀態 slime 正正需要呢種一致縮放。改一兩個常數（`TARGET_W`、狀態名 list）就用得。
> 參考：`tools/prep_character.py:20-123`。

### ⑦ snapshot JSON / status schema / 多角色 manifest 命名（可逐字沿用）〔P1〕
- 心跳快照欄位：`{total, running, waiting, msg, entries[], tokens, tokens_today, prompt{id,tool,hint}}`。
- status 回應：`{name, owner, sec, bat, sys:{up,heap,fsFree,fsTotal}, stats:{appr,deny,vel,nap,lvl}}`。
- 角色資料夾 `characters/<name>/` + `manifest.json`（`name`/`colors`/`states`，state 值=檔名或陣列）。
> 參考：`REFERENCE.md:45-79, 143-155`；`characters/bufo/manifest.json`。

**robot-bridge 補充**：對話階段 phase→顏色 FSM（thinking=rainbow、reply=settled）畀「agent 正在處理」一個清晰即時回饋〔P2〕；`/ota` config bootstrap 解決 Claudi 而家 `claudi.local` / `192.168.4.1` fallback 嘅 IP 困局〔P2〕。

---

## 3. 唔適合直接搬嘅部分（同原因）

| 唔搬嘅嘢 | 原因 |
|---|---|
| **BLE NUS transport + LE Secure 配對/passkey** | Claudi 嘅 hook 係 HTTP 單向推；要用 NUS 就要 host 做 BLE central，但 Claude Code hook 只識 HTTP。保留做*未來第二 transport*，唔好取代 WiFi-HTTP。 |
| **device→desktop `permission` 雙向決策通道** | 好優雅，但要 *Claude 桌面 app 嘅 BLE bridge* 去消費個決策。Claude Code hook 係單向（事件→exit code），`/pet/state` GET 冇 return channel 入返 permission gate。要 custom 一個會 block 等裝置輸入嘅 hook 先得。 |
| **stats 驅動嘅 mood / level / fed-bar / energy** | buddy 嘅 mood 來自 on-device approval 速度 + token 里程碑（每 50K token 升級）。Claudi 嘅 hook 而家**冇 token/cost/approval-latency 來源、冇 IMU**，呢啲 mood 冇 input。除非 Claudi 加 stats feed，否則 skip。 |
| **IMU 驅動狀態（搖→`dizzy`、面朝下→nap）** | 要加速度計。建議**保留 `dizzy` 個名，但改由「錯誤/失敗」觸發**，唔好靠 motion。 |
| **ASCII species 系統（`buddies/*.cpp` 18 隻）** | Claudi 係圖形 slime，唔需要 ASCII art。 |
| **M5 硬件依賴（TFT_eSPI、按鈕、IMU、RTC、M5-NVS）** | Claudi 用 Adafruit GFX + 自己嘅 `Display` 抽象（比 buddy 更乾淨），唔好搬 driver code，只搬概念。 |
| **robot-bridge 嘅 XiaoZhi 語音 / Opus / JPEG / MCP actuator stack** | 同文字寵物完全正交，唔相關。 |
| **`busy` threshold = 3** | buddy 桌面同時開好多 session 先用 ≥3。Claudi 通常 1 session，要改做 **≥1**。 |

---

## 4. 採納優先級（P0 / P1 / P2）

### P0（核心、最高槓桿）
1. **快照式 derived base state + priority ladder**（取代每事件硬 map）。
2. **one-shot overlay（帶 timeout 自動回 base）** 機制。
3. **approval-pending 做最高優先**：force-wake、keep screen、~10s escalate 變紅。
4. **GIF-on-LittleFS 動畫**（加 LittleFS partition + `AnimatedGIF` lib，取代 59KB/幀 raw array）。
5. **`prep_character.py` 全狀態統一-bbox pipeline**（改 `TARGET_W` / 狀態名即用）。
6.（可選 P0/P1）**全螢幕 offscreen sprite + 單次 flush** 消除閃爍——C6 上 172×320×2 = 108KB SRAM 偏緊，S3 有 PSRAM 就無痛。

### P1
7. **transcript HUD + approval card**（tool/hint/elapsed，最新光舊暗、可 scroll）。
8. **新增/重映狀態名**：`busy`（=running，比 `working` 更貼切）、`celebrate`（=任務完成，比 `happy` 更具體）、`dizzy`（=錯誤，spiral eyes 好啱）。
9. **snapshot JSON 欄位 + `/status` schema 逐字沿用**；新增 `POST /snapshot` endpoint。
10. **idle 輪播（array-valued state）**：多段 idle clip loop 完輪換，唔好淨係 loop 一段。
11. **liveness：10s keepalive / 30s 斷線就 sleep**（hook 停就瞓覺）。

### P2
12. **`heart` 微獎勵**（快速 approve 後短暫飄心）。
13. **robot-bridge phase→顏色** 畀「thinking/running」即時回饋。
14. **`/ota`-style config bootstrap**（解決 IP fallback）；**`/internal/*` fan-in**（若 Claudi 將來起一個常駐 host daemon）。
15. **time-of-day mood**（夜晚 sleep、Friday 下午 celebrate）零額外 input 嘅性格。
16. **BLE/HTTP folder-push OTA 換圖**（buddy 嘅 `char_begin/file/chunk/file_end/char_end` 係現成藍圖；Claudi 有 HTTP，仲易過 BLE）。

---

## 5. 可「逐字沿用」嘅具體清單（protocol / endpoint / naming / asset）

### 5.1 Endpoint / Protocol（可直接抄欄位名）
- **新增 `POST /snapshot`**，body 用 buddy 心跳格式：
  ```json
  {"total":3,"running":1,"waiting":1,"msg":"approve: Bash",
   "entries":["...","..."],"tokens":184502,"tokens_today":31200,
   "prompt":{"id":"req_abc123","tool":"Bash","hint":"rm -rf /tmp/foo"}}
  ```
  直接塞入現有 `WebServer` 嘅 `.on()` 表（`src/main.cpp:531-537`）。
- **擴充 `GET /status`** 成 `{sec, sys:{up,heap,fsFree,fsTotal}, stats:{...}}`（drop 走 battery 欄位，map 落 ESP32 heap/uptime）。
- **xfer opcode 集（延後到要傳二進制 asset 先用，可照搬）**：
  `char_begin{name,total} → file{path,size} → chunk{d:base64} → file_end → char_end`，每步 ack `{"ack":<cmd>,"ok":<bool>,"n":<bytesSoFar>}`，`char_begin.total` 做 pre-flight 容量檢查，`file_end` 校驗 `bytesWritten==size`。stop-and-wait + 256B base64 chunk 啱 ESP32 flash。
  > ⚠️ 移植時記得補 path 安全檢查（拒 `..`/絕對路徑）——buddy firmware 原版冇驗（`xfer.h:195`）。

### 5.2 State naming（可沿用 / 重映）
| 沿用 buddy 名 | 用途 | 對 Claudi |
|---|---|---|
| `idle` | 連線中、無急事 | 同 Claudi `idle` 一樣，照用 |
| `busy` | agent running | 比 `working` 更貼切，建議改名 |
| `attention` | approval pending | 保留專指「等 approval」 |
| `celebrate` | 任務完成 | 比 `happy`/`excited` 更具體 |
| `dizzy` | **錯誤/失敗**（重映，唔靠搖） | Claudi 而家冇 error state，spiral eyes 啱晒 |
| `heart` | 快速 approve 後微獎勵 | 新增 |
| `sleep`/`sleepy` | 斷線 / 長時間 idle | 同 Claudi `sleepy` 合併 |

Claudi 獨有、buddy 冇、值得保留嘅幼粒度狀態：`blink, curious, bored, thinking, idea`。

### 5.3 Asset pipeline / 資料夾佈局（逐字沿用）
- 資料夾：`characters/<name>/`
- manifest：`manifest.json`，keys = `name` / `colors{body,bg,text,textDim,ink}` / `states{...}`；state 值 = 檔名字串**或**陣列（陣列=輪播）；可選 `"mode":"text"`。
- 演算法：全狀態 union-bbox crop → 固定闊度 resize → flatten bg → 64 色 quantize → 多幀 GIF（`loop=0, disposal=1`）。
- 縮細秘方：`gifsicle --lossy=80 -O3 --colors 64`（細 40–60%）。
- 上機兩路：USB `pio run -t uploadfs`（寫 `data/characters/<name>/`）或 HTTP/BLE folder-push。

---

## 6. ESP32-S3 適配可能性

Claudi 而家係 **C6**，但若考慮 **S3**：

| 範疇 | C6 現況 | S3 優勢 |
|---|---|---|
| **Framebuffer** | 108KB SRAM offscreen sprite 偏緊（~512KB SRAM） | octal PSRAM `ps_malloc` 多個 framebuffer 無壓力，可做更大 sprite / parallax / 預解碼緩存 |
| **Asset 容量** | LittleFS 受 flash 限 | 大 flash + PSRAM 可同時放多個 GIF 包、預解碼幾幀 |
| **USB** | 原生 USB-Serial/JTAG CDC（單一固定 CDC，冇 TinyUSB descriptor） | 原生 **USB-OTG**，可用 TinyUSB CDC，全速、無 115200 上限；`uploadfs`-over-USB 同 buddy folder-push 更易 |
| **核心** | 單核 RISC-V——GIF decode 爆發可能餓親 WiFi/BLE（buddy 已遇過，single-GIF 狀態凍喺最後一幀避開，`character.cpp:376-386`） | 雙核，decode 同 radio 唔打架 |
| **BLE** | C6/H2 預設 NimBLE | 同樣行 NimBLE-Arduino；buddy 用舊 Bluedroid，`setSecurityAuth/IOCap` 同 NimBLE 1:1 對應；NUS UUID + `mtu-3` chunk notify 同 stack 無關，可直接 port |

> 共通：line-buffered `\n`-delimited JSON dispatch 喺 `Serial`/`USBSerial` 上唔使改就 port 到。LittleFS 喺 C6/S3 一樣。per-chunk ack 嘅理由（flash erase block + 細 RX buffer）喺任何 ESP32 都成立。

**結論**：buddy 嘅 *概念* 全部 port 到 C6/S3；S3 主要係解放 framebuffer/asset 容量壓力 + 雙核避免 decode 餓 radio。若 Claudi 要做全螢幕 offscreen sprite + 大 GIF 包，S3 體驗會明顯順啲。

---

## 7. 最終 Portability Verdict（綜合三個 subagent）

| 借鏡項目 | 借？ | 優先 | 備註 / 出處 |
|---|---|---|---|
| 快照式 derived base state + priority ladder | ✅ | **P0** | 取代每事件硬 map。`main.cpp:479-490` |
| one-shot overlay（timeout 自動回 base） | ✅ | **P0** | transient 反應原語。`main.cpp:487-490,1002` |
| approval-pending first-class + 10s escalate | ✅ | **P0** | force-wake + keep screen。`main.cpp:481,1005-1039` |
| GIF-on-LittleFS（AnimatedGIF）取代 raw array | ✅ | **P0** | 細 10–50×、可 OTA 換圖。`character.cpp:102-136` |
| `prep_character.py` 全狀態 union-bbox pipeline | ✅ 近乎照搬 | **P0** | 改 `TARGET_W`/狀態名。`prep_character.py:20-110` |
| 全螢幕 offscreen sprite + 單 flush | ✅ | **P0/P1** | C6 108KB 偏緊、S3 無痛。`main.cpp:8,954` |
| transcript HUD + approval card | ✅ | **P1** | tool/hint/elapsed。`main.cpp:725-768,890-936` |
| 狀態名 `busy`/`celebrate`/`dizzy(error)`/`heart` | ✅ | **P1** | 語義更貼切。`main.cpp:32-33` |
| snapshot JSON + `/status` schema + `POST /snapshot` | ✅ 逐字 | **P1** | `REFERENCE.md:45-79,143-155` |
| idle 輪播（array-valued state） | ✅ | **P1** | 防單調。`character.cpp:388-398` |
| liveness 10s keepalive / 30s sleep | ✅ | **P1** | hook 停就瞓。`data.h:50-52` |
| manifest.json + `characters/<name>/` 佈局 | ✅ 逐字 | **P1** | 擴成 12 狀態。`characters/bufo/manifest.json` |
| `heart` 微獎勵 | ✅ | **P2** | 快 approve 飄心。`main.cpp:1087` |
| robot-bridge phase→顏色 FSM | ✅ | **P2** | thinking/running 回饋。`ARCHITECTURE.md:206-227` |
| xfer opcode 集（chunk + ack + fit-check） | ✅ 延後 | **P2** | 要傳二進制 asset 先用。`xfer.h:141-232` |
| `/ota` config bootstrap + `/internal/*` fan-in | 🤔 | **P2** | 解 IP fallback / 若起 host daemon。`api.py:109-126,275-333` |
| time-of-day mood | ✅ | **P2** | 零 input 性格。`main.cpp:1168-1181` |
| BLE/HTTP folder-push OTA 換圖 | ✅ HTTP 版 | **P2** | `REFERENCE.md:160-192` |
| NUS BLE transport + LE Secure 配對 | ❌（now） | — | hook 只識 HTTP；留作未來第二 transport |
| device→desktop permission 雙向通道 | ❌ | — | Claude Code hook 單向、無 return channel |
| stats-driven mood / level / energy | ❌ | — | Claudi 冇 token/cost/latency feed |
| IMU 狀態（搖/nap） | ❌（保留 `dizzy` 名） | — | 冇加速度計，改由 error 觸發 |
| ASCII species 系統 | ❌ | — | 圖形 slime 唔需要 |
| M5 driver / TFT_eSPI / 硬件依賴 | ❌ | — | Claudi `Display` 抽象更佳 |
| robot-bridge 語音/Opus/JPEG/MCP | ❌ | — | 同文字寵物正交 |

---

## 附錄：關鍵檔案索引

**claude-desktop-buddy**
- 協議規範：`/tmp/claude-desktop-buddy/REFERENCE.md`
- BLE transport：`src/ble_bridge.cpp:13-180`、`ble_bridge.h`
- JSON parse/dispatch：`src/data.h:70-185`
- xfer 接收：`src/xfer.h:77-239`；發送參考實作：`tools/test_xfer.py`
- 狀態機核心：`src/main.cpp:32-33, 479-490, 988-1019, 1087`
- transcript/approval 渲染：`src/main.cpp:725-768, 890-936`
- asset pipeline：`tools/prep_character.py:20-123`；`tools/flash_character.py`
- manifest 格式：`characters/bufo/manifest.json`
- 角色動畫：`src/character.cpp:102-136, 374-400`；species API `src/buddy.h:25-29`

**robot-bridge**
- 架構 / 對話階段 FSM：`/tmp/robot-bridge/ARCHITECTURE.md:206-227, 374-378`
- WS 二進制 framing：`src/websocket_handler.py:106-122`
- HTTP `/ota` / `/internal/*`：`src/api.py:109-126, 275-333`

**Claudi（現狀）**
- endpoint：`src/main.cpp:531-537`
- hook：`.claude/hooks/claudi_hook.py:46-216`
- 顯示抽象：`include/display.h`、`display_st7789.h`、`src/display_st7789.cpp`
- asset：`include/slime_assets.h`、`src/slime_assets.cpp`
- 板級設定：`include/board_config.h`、`platformio.ini`
