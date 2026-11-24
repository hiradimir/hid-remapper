# hid-remapper-ble (hid-remapper-bt)

## これは何

jfedor2/hid-remapper のフォーク (hiradimir/hid-remapper)。
Raspberry Pi Pico W (RP2040) 1 枚で **BLE HID (マウス / キーボード) を受信し、USB HID に変換して出力するリマッパー**。

上流の機能 (Web configurator によるリマップ設定、ポーリングレート OC など) を維持しつつ、
このフォークで独自に追加した要素は:

- Dual USB Host (Pico-PIO-USB) — 2 ポートの USB デバイス入力
- BLE HID (CYW43 + BTstack) — 8 スロットのホストペアリング管理 (スロット切替 / ペアリングモード / ON-OFF トグル)
- SSD1306 OLED (128x32) 表示 (状態・ポート / スロット情報)
- TLV 形式の Flash 保存 (設定・スロット & ボンディング DB)
- OS 自動判定 (Apple / Android Sticky ガード、ATT MTU 判定) と表示手動切替

多機能 Bluetooth USB ドングルと異なり、ホストからは通常の USB マウス/キーボードに見えるため
専用ドライバ不要。BLE 専用 (Bluetooth Classic 非対応)。

## 開発体制

- ブランチ: `feature/pico-w-ble` (開発ライン)。`master` はフォーク時点上流ベース
- BLE / スロット管理の独自コミットは `ce0369f` (2026-09-27, "Add BLE HID support with Pico W") 以降
- タスク管理: TASKS.md (旧「HID Remapper BLE - 開発管理シート.xlsx」から移植) + HISTORY.md
- 正のドキュメントと上流資料の区別 (AGENTS.md「ドキュメント体系」):
  本フォークの仕様は PROJECT.md/AGENTS.md/TASKS.md/HISTORY.md。
  ルートの README.md / BLUETOOTH.md / HARDWARE.md / SERIAL.md / EXPRESSIONS.md は
  **分岐点 (master) 当時の上流資料そのまま**で一切更新されておらず、
  nRF52840 版 Bluetooth 記述 (BLUETOOTH.md) など本フォークの概念と乖離がある。
  矛盾したら常に正のほうを読む。`firmware-bluetooth/` も上流 nRF52 版の残留 (運用対象外)

## ハードウェア構成

RP2040 Pico W / dual USB host (PIO-USB) / OLED SSD1306 128x32

### ピン割当

| ピン | 機能 |
|------|------|
| GP0 / GP1 | USB Host 1 D+ / D- |
| GP2 / GP3 | USB Host 2 D+ / D- |
| GP4 / GP5 | OLED SSD1306 I2C0 SDA / SCL |
| GP16 / GP17 | UART TX / RX (Debug, 921600 baud) |

### OLED レイアウト (128x32 / 6x8 フォントで 21 文字 x 4 行)

縦 32px をフルに使っており、**5 行目は物理的に作れない**。

| 行 | 内容 |
|---|---|
| Line 0 | トランスポート (BLE-FAST / BLE-SLOW / USB-OUT / NO-HOST) + 通信ヘルス点滅 |
| Line 1 | スロット 1〜4 の状態 (`1>M*` = カレント / 未ボンドは `--`) |
| Line 2 | スロット 5〜8 の状態 (2026-11-24 追加) |
| Line 3 | 通知 (優先順): ホットキー入力 > トースト > USB デバイス ⟷ ピアアドレスの 2 秒フリップ |

- A〜 `>` はカレントスロット。`*` 接続中 / `+` ペアリング中 / `~` 待機中
- Line 3 のフリップは「USB に何が刺さっているか」と「接続中のピアアドレス」を
  2 秒ごとに交互表示する (両方同時に出す横幅がないため)
- WiFi 状態 (BL-39、3 文字) は Line 3 右端に出す。ピアアドレス表示中は
  col18 まで使うので重なりを避けて描かない

### ホットキー仕様 (2026-11-22 プレフィックス方式に変更)

**プレフィックス**: `Cmd + Ctrl + Alt` を押し、**800ms 以内に全部離す**と
ホットキーモードに入る (OLED 最下行が `$             (5s)` になる)。
押す順・離す順は不問。3 つを押したまま別キーを叩いた場合は通常入力として通る。

**モード中**: コマンドを打って **Enter** で実行・終了 (成否に関わらず抜ける)。
スペースの有無は問わない (大文字小文字も無視)。

| 入力 | 動作 |
|------|------|
| `s1` 〜 `s8` | BT スロット切替。カレント指定は `Already Slot N` で無視 |
| `p1` 〜 `p8` | そのスロットの BT ペアリングモード (カレントのみ有効) |
| `clear 1` 〜 `clear 8` | そのスロットのボンド消去 (**移動不要・任意スロット**) |
| `clear all` | ボンド全消去 (Flash DB・コントローラ・解決リスト) |
| `bt` / `bt toggle` | カレントスロットの電波 ON/OFF トグル (OFF 状態は Flash 保持) |
| `bt status` | `Radio:ON  Slot2 Conn` を OLED に表示 |
| `exit` / `e` / `quit` / `q` | モードを抜ける |

**抜け方**: `Esc` で即抜け (Enter 不要)。5 秒無入力でも自動終了 (入力は破棄)。

※ OS 表示の手動切替 (`Cmd+Ctrl+Alt+O`) は旧方式のまま。新しいコマンド体系には
未対応 (必要なら `os` コマンドとして追加する)。

**注**: キーボードレポートは標準 HID の Usage ID 配列ではなく、
Usage 0x04〜0x73 の **112 ビットのビットマップ** (`our_descriptor.cc` の
`Report Count 0x70`)。ホットキー判定はビット位置で行う。

## ビルドオプション

| オプション | 仕様 |
|------------|------|
| ENABLE_BLE | BLE サポート有効化 (Pico W 必須。PICO_BOARD=pico_w 以外では CMake が途中終了) |
| ENABLE_OLED | SSD1306 OLED 表示有効化。デフォルト OFF、`-DENABLE_OLED=ON` で有効化 |

両オプションとも OFF でもビルドが通るインラインスタブ形式 (ble_hid.h / oled.h) を維持。

## コード構成

- `firmware/src/main.cc` — エントリポイント (BL-35 で BLE/OLED の分離済み)
- `firmware/src/remapper.cc` — リマップ処理・送出制御
- `firmware/src/ble_hid.cc/.h` — BLE スタック・スロット管理・ホットキー判定 (8 スロット)
- `firmware/src/oled.cc/.h` — OSD 表示 (BL-33 で接続ハンドシェイク状態 C:E_ K_ M_ 可視化)
- `firmware/src/platform.h` / `tusb_config_*` — 構成別のビルド切替
- Flash 保存: TLV 形式 (`PICO_FLASH_BANK_STORAGE_OFFSET=2084864` にスロット & ボンディング DB)

## ビルド / 検証環境

- devcontainer (.devcontainer/) — workspace は /workspace にマウント
- 通常ビルド (ルディ実運用・devcontainer 内):

    cd /workspace/firmware
    rm -rf build
    mkdir -p build && cd build
    cmake -DPICO_BOARD=pico_w -DPICO_DUAL_PIO_USB_DP_PIN=2 -DENABLE_BLE=ON -DENABLE_OLED=ON ..
    make -j$(nproc) remapper

- `-DPICO_DUAL_PIO_USB_DP_PIN=2` (第二 USB host の D+ を GP2 に指定) は必須
- 成果物: firmware/build/remapper.uf2
- 正確なワンライナー変遷は HISTORY.md / AGENTS.md に運用実績ベースで随時更新

## 参照 (上流 / ベース)

- 上流: https://github.com/jfedor2/hid-remapper (リマップコア、 PIO-USB、Web configurator)
- BTstack (Pico SDK 同梱) — BLE HID peripheral、マルチペアリング
- Web configurator: config-tool-web/ (WebHID 経由の設定・Flash 更新)
