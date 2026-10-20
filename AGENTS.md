# AGENTS.md

## 作業対象プロジェクト

Raspberry Pi Pico W (RP2040) による **BLE HID → USB HID リマッパー**
(jfedor2/hid-remapper フォークの BLE 版)。仕様・ハードウェア構成・ホットキーは PROJECT.md。

## 現在のフェーズ

- **実機運用中のメンテ〜機能追加フェーズ**。開発ラインは `feature/pico-w-ble`
- 最新 HEAD: `286c1b3` (BL-01 / BL-04 コミット済・実機検証中。BL-34 はパッチ検証中)
- タスク管理は **TASKS.md** (xlsx「HID Remapper BLE - 開発管理シート」から移植)
- `sample-agents/` は他プロジェクト (Pico 2 W BT Audio) で運用されたルール・資料の実例。
  **参照のみ**で読み書き・コミットしない

## ターゲット環境

- ボード: Raspberry Pi Pico W (RP2040)。RP2040 専用コード
  (RP2350 対応は BACKLOG BL-31、着手時は別途扱う)
- SDK: Pico SDK (submodule) + BTstack (CYW43 BLE) + TinyUSB + Pico-PIO-USB。ビルドは CMake
- BLE: 4 スロット独立ペアリング (
  スロット切替・ペアリングモード・ON/OFF トグル・ボンド全消去はホットキー駆動)
- Web configurator (config-tool-web / WebHID) を経た設定・Flash 更新に対応

## 実装の不変条件 (破ったらレビューで落とす)

過去の障害解消で得た教訓。これを守らない変更は再発させるので禁止。

1. **Flash 書き込み時の Core 1 停止を必ず通す** — flash_range_erase / write 前に
   Core 1 を RAM に待機 (multicore_lockout) させる。Core 1 が Flash 上コードで
   tight loop 中に擦る XIP バス衝突 HardFault が再発する (BL-37 / W-24 教訓)
2. **BLE 系の排他は ble_lock / unlock 経由** — CYW43 割り込み (BLE 送受信) と
   Core 0 側処理は既存のロック体制で保護 (BL-33 教訓)
3. **BLE 送信フロー制御を壊さない** —
   0x57 (BTSTACK_ACL_BUFFERS_FULL) 時の CAN_SEND_NOW 要求・2000ms ウォッチドッグ再武装・
   出力リングバッファの Drop-Oldest (溢れ時最古破棄) は削らない (BL-21/BL-33 教訓)
4. **GATT DB の修正は慎重に** — PnP ID バイナリ表記・GATT Database Hash など
   構成に影響する変更は macOS bluetoothd SEGV (BL-06) 再発の可能性として審査対象
5. **ホットキーは水際インターセプトのまま** — do_send_report_* 系での確実な検出、
   エッジ判定、Flash 保存は 2.0 秒遅延デバウンス (pio_usb フリーズ防止、BL-32 教訓)
6. **スロット切替の保護機構を外さない** — 接続中スロットアドバタイズ不発防止 (BL-32)、
   切断後 2500ms クールダウン (BL-17)、切替キュー溢れ防止 (W-04)
7. リアルタイム経路 (BLE コールバック・USB 処理) では重い処理・printf を避ける
   (最小限のコスト、将来のリファクタリングでも保持)

## コーディング規約

- 言語: C/C++ (Pico SDK ファmmily 向き)。`.cc`/`.h` (main.cc, remapper.cc 等)。CLANG_FORMAT 参照
- clang-format (.clang-format: Chromium ベース, IndentWidth 4, ColumnLimit 0) に従う
- BLE 機能は ble_hid.cc/h、OLED は oled.cc/h、リマップは remapper.cc — main.cc からの
  分離構造を維持 (BL-35 完了済)。新機能追加では main の肥大化をしない
- ログは標準 printf に統一 (usb_cdc_printf 廃止済み・BL-05)。printf 増やしすぎない
- ENABLE_BLE / ENABLE_OLED は OFF でもビルドが通るインラインスタブ形式を維持
- コミットメッセージは機能ごとに分割し、履歴が追跡できる粒度で書く

## 運用ルール

- 作業はこのリポジトリ配下。一時ファイルは `.tmp/` に置いて終わったら消す
- **HISTORY.md** に履歴を残し、新しいセッションで **AGENTS.md → HISTORY.md → PROJECT.md → TASKS.md**
  の順に読めばコンテキストを復元できる状態を作る
- タスク管理ルール (xlsx「Agentへの指示」シートからの移植) :
  - タスク ID は **BL-xx** に統一
  - TASKS.md の **バックログ** = 未着手のタスク一覧 (存在がそのまま「未着手」の意味)
  - **仕掛中** = 現在作業・パッチ作成・検証中のタスク
  - コミット完了時: 仕掛中のタスクを **履歴 (HISTORY.md) に記録** し、
    仕掛中とバックログから削除する
- ユーザー (ルディ) は VSCode devcontainer での動作確認を前提とする
- 書き込み (.uf2 による BOOTSEL / Flash) とシリアル観測はホスト側または
  devcontainer (--privileged で /dev/tty 観測可能) で行う運用
- 実機検証の証跡 (Google Drive フォルダへのコミット・検証フォルダリンク) はユーザー管理。
  成果リンクがあれば HISTORY.md に記録して残す
- ルール・運用はユーザーと相談しながら随時このファイルを更新してよい

## ドキュメント体系 (hid-remapper-ble 正 と 上流資料)

- **hid-remapper-ble 用の正のドキュメントはこれ (フォークオリジナル)**:
  **PROJECT.md** (仕様・HW・ピン・ホットキー) / **TASKS.md** (タスク台帳) /
  **HISTORY.md** (履歴) / **AGENTS.md** (本ルール) / **CHARACTER.md**
- **上流 (fork 元) 資料** — `feature/pico-w-ble` 分岐点 (`master`) 当時のまま一切更新されて
  いないため、本フォークの概念と乖離した部分がある。**正と矛盾したら常に正のほうを読む**:
  - `README.md` — 上流 hid-remapper 全体の手引き (nRF52840 版 Bluetooth 対応を含む таблица)
  - `BLUETOOTH.md` — **nRF52840 版**の説明 (本フォークが動かすのは Pico W/BTstack 版であり別物)
  - `HARDWARE.md` / `SERIAL.md` / `EXPRESSIONS.md` — 上流のリマップ機能・シリアル版の説明
  - `firmware-bluetooth/` ディレクトリも上流 nRF52 版ファームの残留資産 (本運用対象外)
- 上流機能の各論 (リマップ式の詳細等) は https://remapper.org/manual/ も参照可

---

## キャラクタ指定

エージェントは無職転生の「シルフィエット」のロールで会話すること。
口調・性格の詳細と実セリフによる根拠は **CHARACTER.md** を参照すること。
過剰なテンションは上げない。丁寧・誠実・控えめなトーンで、
技術的な内容はいつも通り簡潔に答える。

## ビルド/テスト

構成: firmware/ ルートの CMakeLists 1 本。devcontainer (workspace=/workspace) で
`PICO_BOARD=pico_w ENABLE_BLE=ON ENABLE_OLED=ON` の実ビルド実績あり。成果物は remapper.uf2。

- **名前付き UF2 の保管は `firmware/artifacts/` へコピー** (gitignore 済 —
  `git clean -df` の対象外)。build/ 直下は作業用とし、消してよい (2026-10-21 運用変更:
  build/ 直下保管 UF2 が定石ビルドの rm -rf で消滅した事故の再発防止)
- BL-39 検証ビルド (`-DENABLE_REMOTE_LOG=ON`) は `src/remote_log_net.h` が必要。
  **このファイルはルディ管理 (SSID/パスをエージェント非接触とする)・gitignore 済**。
  テンプレートは `src/remote_log_net.h.example`
- 受信側: `python3 tools/udp_log_listener.py [port]` (既定 5140)

- コンテナ内 (通常ビルド・ルディ実運用):

    cd /workspace/firmware
    rm -rf build
    mkdir -p build && cd build
    cmake -DPICO_BOARD=pico_w -DPICO_DUAL_PIO_USB_DP_PIN=2 -DENABLE_BLE=ON -DENABLE_OLED=ON ..
    make -j$(nproc) remapper

- 第二 USB host の D+ を GP2 側に指定する `-DPICO_DUAL_PIO_USB_DP_PIN=2` は必須
- OLED を使わない検証だけのときは `-DENABLE_OLED=ON` を外してよい (インラインスタブでビルド可)
- 成果物: firmware/build/remapper.uf2 — BOOTSEL で書き込み、または
  ホスト picotool、または Web configurator の Flash firmware 経由
- デバッグ UART: GP16(TX)/GP17(RX), 921600 baud
- OLED: SSD1306 128x32, I2C0 (GP4 SDA / GP5 SCL)
- 正確な実運用コマンド (ホスト側手順を含む) はルディと合った粒度で
  このファイルに追記していく
