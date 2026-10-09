# DeaDBeeF with 3D OpenGL Cover Flow Plugin

[![License: GPL v2 / ZLIB](https://img.shields.io/badge/License-GPLv2%20%2F%20ZLIB-blue.svg)](COPYING)
[![Platform: Linux](https://img.shields.io/badge/Platform-Linux%20%28x86__64%29-orange.svg)]()
[![GTK Version: GTK3](https://img.shields.io/badge/GTK-3.0-green.svg)]()
[![OpenGL: 3D Hardware Accelerated](https://img.shields.io/badge/OpenGL-Hardware%20Accelerated-red.svg)]()

**DeaDBeeF** 是一款在 Linux 上廣受推崇、極致輕量、啟動飛快且高度模組化的終極音樂播放器。

本專案為 DeaDBeeF GTK3 介面深度整合了原生的 **3D OpenGL Cover Flow 視差專輯封面外掛** 與專為 Linux Mint MATE 深度優化的 **OSD Notify 桌面快訊通知外掛**，重現如經典 iTunes 般華麗流暢的立體翻頁互動，並針對現代高解析度寬螢幕（1080p+）、深色主題（Dark Theme）以及多螢幕桌面環境進行了全方位的視覺架構重構與體驗優化。

---

## 📸 視覺效果預覽 (Screenshot)

![DeaDBeeF Cover Flow 3D](./CoverFlow.png)

---

## ✨ Cover Flow 核心特色 (Features)

- 🚀 **原生 OpenGL 硬體加速渲染**
  基於 `GtkGLArea` 與 `libepoxy` 實作現代 OpenGL 渲染管線，具備 60 FPS 流暢物理慣性滑動與立體視差翻頁動畫。
- 🔄 **播放與清單選取即時雙向同步（Two-way Playback & Selection Sync）**
  - **歌曲切換自動對焦**：當自動播放下一首、切換曲目或點選上一首/下一首時，Cover Flow 鏡頭自動平滑滑動並置中目前播放歌曲所屬的專輯。
  - **清單選取即時跟隨**：在播放清單中單擊或移動游標選取不同歌曲時，3D 舞台立即同步滑動對齊該專輯，無須在龐大曲庫中手動翻找。
  - **雙擊播放無縫流暢**：雙擊 3D 封面即刻播送，封面完全不閃爍、不短暫消失。
- 🖼️ **長寬比自動維持與全彩支援（Aspect Ratio Preservation & TrueColor）**
  完美避免封面圖形拉伸或變形；支援各類非正方形封面，邊緣採用高質感透明漸變與深色底層無縫融合，並徹底解決色相扭曲與裁切偏移問題。
- 🏷️ **智慧專輯聚合（Smart Album Grouping）**
  智慧依據 `Album Title` 聚合曲目與專輯封面，多音軌或同名專輯自動歸整，杜絕重複圖示。
- 🎯 **浮動標題抬頭顯示（HUD Overlay）**
  選中的專輯名稱以半透明膠囊造型優雅懸浮於 3D 封面正上方，移除下方佔空間的傳統統計文字，最大化視覺工作區域。
- 💡 **立體光影與焦點突顯（Ambient Dimming & Dynamic Scaling）**
  中央當前選取的專輯封面以全光照耀並突顯；兩側未選中的專輯自然微縮並降低亮度（Ambient Dimming），營造深邃立體景深。
- 📐 **寬螢幕舒適非密集間距（Comfortable Staggered Spacing）**
  元件預設請求至少 $1080\text{px}$ 寬度視窗，中央與兩側封面間距經過精密設計，向兩側自然延伸並維持開闊間距，不再密集擁擠重疊。
- 🌙 **全域深色沉浸主題（Full Dark Theme Integration）**
  完整適配深色模式，CoverFlow 3D 舞台、播放清單分頁標籤、表格標頭欄位與選單皆呈現一體化的純黑高對比沉浸設計。
- ⚡ **非同步快取與低資源消耗**
  背景多執行緒非同步載入封面材質，即便面對數萬首歌曲的大型曲庫也能秒開且滑動不掉格。
- 🎨 **播放清單右鍵快速批次編輯封面與專輯名稱**
  在播放清單中選取單首或多首曲目並按右鍵，即可使用「更改或插入封面圖片 (Cover Art)」與「批次更改專輯名稱 (Album Title)」，支援 PNG/JPG 圖檔即時預覽並自動更新 CoverFlow 3D 舞台呈現。

---

## 🔔 OSD Notify 桌面通知外掛增強 (Desktop Notification Plugin)

專案內建經過現代化升級重構的 `notify` 外掛，針對 Linux Mint MATE、Cinnamon、XFCE 及各大 Linux 桌面環境進行了相容性與美化強化：

- 🖼️ **Cover Art 縮圖支援**
  同時傳遞 FreeDesktop Spec 1.1/1.2 標準之原始點陣圖數據 (`image_data`/`image-data`) 與檔案路徑 (`image_path`/`image-path`)，確保在 `mate-notification-daemon` 等通知守護行程中皆能完美顯示專輯封面縮圖。
- ⏱️ **通知顯示時間自由調整 (Notification Duration)**
  偏好設定提供 `1 ~ 60` 秒數值微調選項（預設 5 秒），時間到達自動淡出關閉，避免通知長時間殘留。
- 📐 **X11 視窗強制定位攔截器 (Vertical Offset / Y Position)**
  - **痛點解決**：在 Linux Mint MATE 等環境中，通知常駐程式原生忽略 D-Bus 的座標提示，且多螢幕下工作區計算容易將通知固定在頂部 `y = 6`，直接覆蓋並擋住頂部的 `mate-panel`。
  - **智慧定位機制**：外掛內建 X11 視窗定位器，於通知彈出時精確偵測通知視窗並透過 `XMoveWindow` 將其平移至指定高度（預設 48 px，避開 31 px 面板）。
- 🔤 **緊湊字型排版模式 (Use Small Font Size)**
  可選取使用 `<small>` 標籤渲染通知內容，有效節省螢幕空間，呈現精緻視覺。

---

## 🎬 Video & CoverArt 播放外掛 (MP4 視訊與專輯封面播放元件)

專案提供基於 `libmpv` 的 **GTK3 視訊與封面多功能播放元件 (`video_player`)**：

- ⚡ **Nvidia NVDEC 專用硬體加速**
  底層配置 `hwdec=nvdec,nvdec-copy,auto`，深度調用 Nvidia GPU NVDEC 解碼引擎，順暢播放 1080p / 4K MP4、MKV、WebM 等主流視訊格式，極致降低 CPU 負載。
- 🎵 **雙重音效阻斷與精準 A/V 影音同步 (Zero Audio Conflict)**
  外掛主動關閉 libmpv 音訊輸出軌（`aid=no`），所有音訊完整保留由 DeaDBeeF 音訊管線、DSP 等化器與音量控制輸出；外掛內部每 500ms 監聽時鐘位移，軟體級無縫校正視訊進度。
- 🖼️ **非影片音樂檔自動呈現 CoverArt**
  當播放非影片音樂檔（MP3、FLAC、WAV、AAC 等）時，視訊區域自動切換為高質感專輯封面視窗，支援長寬比維持縮放、立體陰影與置中排版。
- 🖥️ **一鍵雙擊全螢幕放大 (Fullscreen Toggle)**
  - **滑鼠雙擊**：在視訊或封面畫面上按滑鼠左鍵雙擊（Double Click），瞬間平滑放大至全螢幕。
  - **快捷鍵支援**：支援 `F11` 或 `F` 鍵切換全螢幕，按 `Esc` 立即退回嵌入視窗。
  - **游標自動隱藏**：進入全螢幕後，滑鼠靜止 2 秒自動隱藏游標，移動滑鼠即刻恢復。

---

## 🎨 批次維護專輯封面與名稱 (Batch Cover Art & Album Title)

為了讓您的音樂庫在 Cover Flow 3D 舞台中精確分類、完美呈現，本外掛為播放清單擴充了直覺的右鍵快捷選單：

1. **選取曲目**：在播放清單中按住 `Ctrl` 或 `Shift` 點擊同一張專輯的一首或多首曲目。
2. **右鍵開啟選單**：在選取的曲目上按滑鼠右鍵，即可點選：
   - **更改或插入封面圖片 (Cover Art)...**
     - 支援所有主流 **PNG** 與 **JPG / JPEG** 圖檔格式。
     - 檔案選擇器內建 **即時圖片預覽 (Live Preview)**。
     - 自動批次將圖檔套用至音檔目錄（儲存為標準 `cover.jpg/png` 與 `folder.jpg/png`）。
     - 即刻清除舊快取，Cover Flow 3D 封面瞬間同步更新！
   - **批次更改專輯名稱 (Album Title)...**
     - 一鍵批次修改所有選取曲目的 `Album Title` 標籤並同步寫入音訊檔案中。
     - Cover Flow 將立即依據新的專輯名稱將音軌歸整至同一個 3D 專輯封面下。

---

## 🎮 操作方式 (Controls & Navigation)

| 操作方式 | 功能說明 |
| :--- | :--- |
| **滑鼠滾輪 (Scroll Up / Down)** | 向左 / 向右平滑翻動專輯 |
| **滑鼠左鍵點擊 (Single Click)** | 選中並將該專輯平滑滑動至舞台中央 |
| **滑鼠左鍵雙擊 (Double Click)** | 立即將該專輯載入播放，並從第一首歌曲開始播送 |
| **滑鼠按住拖曳 (Drag & Slide)** | 依拖曳速度自由平滑滑動瀏覽整座專輯庫 |
| **鍵盤方向鍵 (Left / Right Arrow)** | 往左 / 往右逐一切換相鄰專輯 |
| **鍵盤 Enter 鍵** | 立即播放目前中央選中的專輯 |

---

## 🖥️ 系統匣常駐與命令列視窗喚醒 (Tray Residency & CLI Activation)

本專案提供基於 UNIX Domain Socket IPC 的 **原生 C 語言啟動器 (`deadbeef-coverflow`)**，完美支援 MATE Desktop / 系統匣常駐與視窗精準控制：

| 執行指令 | 運作情境 | 行為說明 |
| :--- | :--- | :--- |
| `deadbeef-coverflow --show` | **已在背景常駐** | 透過原生 IPC 即刻叫出主視窗並置頂（`gtk_window_present`）。 |
| `deadbeef-coverflow --show` | **尚未在背景執行** | 啟動播放器，同時保持常駐在 mate-panel 系統匣且**立即顯示主視窗**。 |
| `deadbeef-coverflow`（無參數） | **開機自動啟動 / 終端執行** | 保持靜默常駐於 mate-panel 系統匣，**不顯示主視窗**。 |

* **應用程式選單整合**：`deadbeef-coverflow.desktop` 預設採用 `Exec=deadbeef-coverflow --show %F`，從選單點擊即可隨時快速叫出主視窗。
* **開機自動啟動整合**：`~/.config/autostart/deadbeef.desktop` 採用 `Exec=deadbeef-coverflow`，開機登入後自動常駐於系統匣，不干擾桌面。

---

## 🛠️ 安裝與建置方式 (Installation & Build)

本專案提供多種靈活的安裝與建置方式：

### 方法一：安裝獨立打包好的 Debian / Ubuntu 套件（推薦）

若您使用的是 **Ubuntu 24.04 (noble)**、**Linux Mint 22.x** 或相容的 Debian 系列系統，可以直接安裝已編譯好的獨立 deb 套件。
本套件命名為 `deadbeef-coverflow`，安裝在獨立路徑，**絕不衝突** 系統原有的官方 `deadbeef` 套件：

```bash
# 1. 直接安裝預編譯 package
sudo dpkg -i deadbeef-coverflow_1.10.3-1~mint22.3_amd64.deb

# 2. 啟動播放器（顯示主視窗並常駐系統匣）
deadbeef-coverflow --show

# 3. 靜默常駐於系統匣（不跳出主視窗，適用開機自動啟動）
deadbeef-coverflow
```

若您修改了原始碼，也可以隨時一鍵重新打包 deb：
```bash
./build_deb.sh
```

---

### 方法二：使用獨立腳本編譯外掛（適用現有 DeaDBeeF）

若您的系統上已經安裝了現有的 DeaDBeeF（例如透過 PPA、APT 或 Tarball 安裝），無需重新編譯整套播放器核心，只要使用獨立腳本編譯 Cover Flow 與 Notify 外掛本體即可：

#### 1. 安裝編譯相依套件
```bash
sudo apt update
sudo apt install -y build-essential pkg-config libgtk-3-dev libepoxy-dev libdbus-1-dev libx11-dev libgdk-pixbuf2.0-dev
```

#### 2. 執行獨立編譯腳本
```bash
# 一鍵編譯 Cover Flow 與 Notify 外掛並自動安裝至當前使用者的外掛目錄 (~/.local/lib/deadbeef)
./build_plugin.sh
```

`build_plugin.sh` 支援的進階選項：
```text
選項：
  -i, --install          安裝外掛至使用者目錄 (~/.local/lib/deadbeef) [預設]
  -s, --system           安裝外掛至全系統目錄 (/usr/lib/deadbeef，需 root/sudo)
  -d, --dest <DIR>       安裝至自訂目錄
  -n, --no-install       僅編譯產生 .so，不執行複製
  -c, --clean            清理編譯暫存檔
  -h, --help             顯示說明訊息
```

---

### 方法三：從原始碼編譯全套 DeaDBeeF

若需自源碼編譯包含全部外掛的完整 DeaDBeeF：

```bash
# 1. 建立組態
./configure --prefix=/usr

# 2. 平行編譯
make -j$(nproc)

# 3. 安裝
sudo make install
```

---

## 💡 如何在 DeaDBeeF 中啟用 Cover Flow 元件與設定 Notify

### 啟用 3D Cover Flow：
1. **啟動播放器**：執行 `deadbeef` 或 `deadbeef-coverflow`。
2. **進入設計模式**：在頂部主選單點選 **檢視 (View)** -> 勾選 **設計模式 (Design Mode)**。
3. **新增 Cover Flow 元件**：
   - 在主畫面任一分割視窗或空白處點擊滑鼠右鍵。
   - 選擇 **插入新元件 (Insert New Widget)** -> 點選 **Cover Flow**。
4. **退出並儲存佈局**：再次點選選單中的 **檢視 (View)** -> 取消勾選 **設計模式 (Design Mode)**。
5. **開啟深色主題（建議）**：
   - 點選 **編輯 (Edit)** -> **偏好設定 (Preferences)** -> **GUI** 或 **外觀 (Appearance)**。
   - 勾選深色主題選項，即可享受與 Cover Flow 一體化沉浸式純黑視覺。

### 設定 OSD Notify 通知：
1. 開啟 **編輯 (Edit)** -> **偏好設定 (Preferences)** -> 切換至 **外掛 (Plugins)** 分頁。
2. 在外掛清單中點選 **OSD Notify** 並點擊下方 **設定 (Configure)** 按鈕。
3. 可自由調整：
   - **Notification duration (seconds)**：調整快訊停留時間（預設 5 秒）。
   - **Vertical offset / Y position (px)**：調整垂直顯示座標（預設 48 px，避開面板）。
   - **Show album art**：開啟/關閉專輯封面縮圖。
   - **Use small font size**：切換為緊湊縮小字型。

### 啟用 Video & Cover Player 視訊/封面元件：
1. **進入設計模式**：在頂部主選單點選 **檢視 (View)** -> 勾選 **設計模式 (Design Mode)**。
2. **新增元件**：
   - 在主畫面任一分割視窗或空白處點擊滑鼠右鍵。
   - 選擇 **插入新元件 (Insert New Widget)** -> 點選 **Video / Cover Player**。
3. **退出並儲存佈局**：再次點選選單中的 **檢視 (View)** -> 取消勾選 **設計模式 (Design Mode)**。
4. **享受極致影音與全螢幕體驗**：
   - 播放 MP4 視訊：自動啟動 Nvidia NVDEC 硬體加速，零延遲輸出高畫質影片。
   - 播放純音樂歌曲：自動呈現目前播放歌曲之專輯封面 CoverArt。
   - 雙擊畫面或按 `F11` / `F` 鍵即可放大至全螢幕，按 `Esc` 鍵立即退回。

---

## 📦 系統與相依需求 (Prerequisites)

### 編譯外掛必備：
- **GCC / Clang** (支援 C99/C11)
- **Make**
- **pkg-config**
- **GTK+ 3.0 開發檔** (`libgtk-3-dev >= 3.20`)
- **Epoxy OpenGL 函式庫開發檔** (`libepoxy-dev >= 1.4`)
- **D-Bus 開發檔** (`libdbus-1-dev >= 1.10`)
- **X11 函式庫開發檔** (`libx11-dev`)
- **GdkPixbuf 開發檔** (`libgdk-pixbuf-2.0-dev`)

### 執行期環境：
- 支援 OpenGL 2.1+ / OpenGL 3.0+ 之顯示卡驅動程式（Intel、AMD、NVIDIA 均可）。
- 支援 D-Bus 與 FreeDesktop 標準通知之桌面環境（MATE、Cinnamon、GNOME、XFCE、KDE 等）。

---

## 📄 授權條款 (License)

- **DeaDBeeF 核心與基礎模組**：採用 [ZLIB License](COPYING)。
- **Cover Flow 外掛元件**：採用 [GNU General Public License v2 (GPLv2)](COPYING.GPLv2)。
- **OSD Notify 外掛元件**：採用 [GNU General Public License v2 (GPLv2)](COPYING.GPLv2)。
