# HISTORY.md

新しいセッションは AGENTS.md を読んだ後にこのファイルで経緯を追うこと。
新しい出来事は上に追記する。コミット完了時は TASKS.md の運用ルールに従い
仕掛中 → この履歴へ記録 → TASKS.md から削除する。

※ 本ファイルの初期内容は旧管理シート (xlsx「履歴」シート) の移植。
旧 ID 表記 (W-xx) は当時のもの。現在は BL-xx に統一。

---

## 2026-10-18

- **cli セッション 1 初 days (シルフィ・opencode)**: 運用を cli エージェント (シルフィ) セッションへ移行。
  旧管理シート xlsx「HID Remapper BLE - 開発管理シート」を sample-agents/ に置き、
  内容を **AGENTS.md / PROJECT.md / TASKS.md / HISTORY.md / CHARACTER.md** へ移植した
  (全てルート直下・現状 **未コミット**)。以後のタスク台帳・履歴の主はこの *.md 群。
  sample-agents/ は参照のみ (原版 xlsx 含む)
- ドキュメント体系を確定 (AGENTS.md「ドキュメント体系」):
  **本フォークの正は PROJECT/AGENTS/TASKS/HISTORY/CHARACTER.md**。
  ルートの README/BLUETOOTH/HARDWARE/SERIAL/EXPRESSIONS.md は `master` 分岐点当時の
  上流資料のまま一切未更新 (BLUETOOTH.md は nRF52840 版)、`firmware-bluetooth/` も
  上流 nRF52 版の残留。矛盾時は常に正のほうを読む
- 通常ビルドコマンドを確定・記録 (AGENTS.md / PROJECT.md、ルディ実運用):

    cd /workspace/firmware && rm -rf build && mkdir -p build && cd build
    cmake -DPICO_BOARD=pico_w -DPICO_DUAL_PIO_USB_DP_PIN=2 -DENABLE_BLE=ON -DENABLE_OLED=ON ..
    make -j$(nproc) remapper

- **BL-34 を仕掛中から進行へ**: 旧 agent 生成の stash パッチ (OLED スクリーンセーバー +
  Toast + カウントダウン) を pop して全 diff 評価した。
  - 良い点: 3段階減光 (0x8F→0x30→0x05) + バウンスロゴ、Toast (スロット切替/ペアリング/
    電波ON・OFF/OS切替/全消去)、ペアリング 60s・再接続 30s カウントダウン、
    スロット記号 (* + ~ -) の精緻化 — 方向性は TASKS.md 仕様どおりで**維持決定**
  - 修正した 3 点 (作業ツリーへ反映済み):
    1. Line 3 が 22 文字 (132px) で 128px 超え → 末尾 "s)" 切欠け (= 「表記乱れ」の
       第一疑い)。`Pairing Mode   (%2ds)` / `Connecting...  (%2ds)` に 21 文字内へ修正
    2. oled.cc の `extern "C" ... ble_hid_get_conn_interval` 宣言ハックを廃止、
       クリーンヘッダ `#include "ble_hid.h"` に変更
    3. `reconnect_start_us` 未適用経路の `( 0s)` 抜けを塞ぐ (500ms タイマでフォールバック起算)
  - ビルド検証済: ENABLE_BLE=ON/ENABLE_OLED=ON (DP_PIN=2) で remapper.uf2 生成・
    warning 0、ENABLE_BLE=OFF スタブ構成もリンクまで OK。
    参考情報: ホストに Arm GNU Toolchain 15.3 あり (cmake でホストビルド可)。
    oled.cc は CMake で ENABLE_BLE ブロック配下 (BLE 無効時は oled.cc 自体が未コンパイル)
  - **残: 実機フラッシュ検証** (スクリーンセーバー遷移・カウントダウン・乱れ消滅確認)。
    乱れが消えない場合は BL-37 (I2C 100kHz 更新ムラ/常在要素) 絡み → リライト指示案の再検討
  - 作業ツリー状態: ble_hid.cc / main.cc / oled.cc / oled.h に BL-34 パッチ+上記修正あり
    (**未コミット・stash は空**)。検証 OK なら BL-34 コミット → TASKS.md 反映

## 2026-10-12

- **286c1b3: BL-01 / BL-04 コミット (実機検証中)**
  OS 自動判定 (Apple/Android Sticky ガード、ATT MTU 判定)、
  Cmd+Ctrl+Alt+O による OS 表示手動切替、BT-OFF 状態の Flash 保持、
  2 秒デバウンス保存統合
  フォルダ: https://drive.google.com/drive/folders/1HB0nPJmmVrszHmqsC-BH7_XX_Wke7V8k
- ef1ddc0: **BL-35 完了** — BLE 機能・OLED 機能の main.cc からの完全分離リファクタリング。
  トランスポート制御 (prev_ble_connected、マウス pacing、送出分岐)、ホットキー判定
  (ble_hid_check_hotkey)、キー離下処理を ble_hid へ集約。OLED の USB ポート種別判定
  (classify_device_type) と監視タイマーを oled へ移譲。TinyUSB と BTstack のヘッダ競合解消
  (C ヘルパー経由)、ブートバナー出力適正化、main 初期 sleep 削除
  フォルダ: https://drive.google.com/drive/folders/1K7ASXPNgawFHAMpQHowgWnHbK08_pk_6

## 2026-10-09

- 269084a: **BL-32 対応完了** — 接続中スロットから別スロットペアリング突入時の
  アドバタイズ不発防止・ホットキー検出判定改善・Flash 競合防止。
  ホットキー水際インターセプト (do_send_report_*) による確実なスロット切替、
  エッジ検出改善、2.0 秒遅延 Flash 保存による pio_usb フリーズ根絶
  フォルダ: https://drive.google.com/drive/folders/1nqSuPPW1vRQPWAxKX34y1fXFeitrG12C
- 7cb6ba9: **BL-38** 接続状態 OLED 可視化 (C:E_ K_ M_) 導入 + 3 ホスト切替検証。
  接続ハンドシェイク (暗号化 E / KBD 購読 K / Mouse 購読 M) の可視化。
  Mac (Slot 1) / Win (Slot 2) / Android (Slot 3) のランダム切替と
  電波 ON/OFF トグルで安定動作を確認
  フォルダ: https://drive.google.com/drive/folders/1TmbSJnqPiGW5WPpZq43k8RJjZGeGWTCk

## 2026-10-08

- 8e2ad76: **BL-33 完了** — BLE 送信フロー膠着 (CAN_SEND_NOW スタール) と
  ホットキー無反応の解消。Core 0 と CYW43 割り込みの排他ロック (ble_lock/unlock) 導入、
  メインループ冒頭でのホットキー先行処理、2 回連続 CAN_SEND_NOW タイムアウト (4000ms) 時の
  gap_disconnect 自動復旧、macOS スレーブレイテンシ抑制、接続安定化重複防止
  フォルダ: https://drive.google.com/drive/folders/1ZUoZIzU8yY3AkOIUtOxrzk-edhlfwlk-

## 2026-10-05

- 399d501: **BL-17 / BL-29 / BL-30 完了** — adv_step_timer 撤廃によるアドバタイズ
  一括同期設定・不発解消 (BL-29)、ボンド全消去ホットキー NKRO 判定是正・全 DB エントリ消去
  (BL-30)、同一ホストの複数スロットペアリング時の即時切断・他スロット登録保護 (BL-17)、
  切断後 2500ms 広告停止クールダウンによる macOS 過剰リトライストームの収束
  フォルダ: https://drive.google.com/drive/folders/1_Htly5ThjrRDlUlEka19tSlEeZXXG6qB

## 2026-10-03

- 36310f1: **W-21 (BL-06) 完了** — macOS bluetoothd SEGV の根本修正
  (PnP ID バイナリ表記適正化、GATT Database Hash 化) によるペアリング時 BTイヤホン
  瞬断解消。ペアリング突入時のスロット個別 DB 消去、500ms クールダウンタイマ、
  接続後 100ms 送信安定化ガード
  フォルダ: https://drive.google.com/drive/folders/1mnH2TR3cUPcuX4OXQAtTu2Mw9ltShCkx
- 40a3d5d: **W-24 完了** — Web-config 保存時の Core 1 Flash 競合 HardFault (0x1400000c)
  解消。do_persist_config() を RAM 常駐化し multicore_lockout + WFE で保護
  フォルダ: https://drive.google.com/drive/folders/125zsnhuTWi90yEHV3ny0Gyu_idKZeDl8
- f4bef17: **W-25 (BL-21) 完了** — BLE フロー制御デッドロック解消
  (0x57 BTSTACK_ACL_BUFFERS_FULL 時の CAN_SEND_NOW 要求＋ 2000ms ウォッチドッグ再武装)。
  出力リングバッファ 8→32 拡張と Drop-Oldest (溢れ時最古破棄) で
  overflow! 連打・ホットキー破棄を根絶
  フォルダ: https://drive.google.com/drive/folders/1uRUWrszZGHgdhQQWsT1Foq3ygjgGiASs
- (記録) **BL-27**: マウス速度変化 (高ポーリングレート化・4fps ループ解消) の仕様確定
- 2e5cfb7: **W-22 (BL-23)** スロット切替時の高速再接続改善・再接続 30 秒タイムアウト・
  MAC ソルト更新
  フォルダ: https://drive.google.com/drive/folders/1C6Ukd1pqUHsqYgVzu_62xoBwmVsriILI

## 2026-10-02

- 41cf79d: **W-13〜W-17 完了** — 同一スロット切替スルー、P1/P2 個別認識・HID 種別判定是正、
  Line 1 アイコン化、Line 4 BT アイコン左寄せ、BLE 接続時モード維持
  フォルダ: https://drive.google.com/drive/folders/1gF2iGBaSna3IHBOoOUHm5a4e4qoPHqtM
- 38524fe: **W-18 (BL-05) 完了** — usb_cdc_printf 廃止・標準 printf への一本化、
  マクロ/extern 宣言削除、デバッグコメント清掃
  フォルダ: https://drive.google.com/drive/folders/1srf2DMSeZCR8fHq2ycvL227xNUHrx7UW
- b95b09d: **W-20 (BL-14, BL-15)** 統一ステートマシンと送信制御 — スロット切替・
  再起動時の Mac BT クラッシュ解消。暗号化＆購読完了までの送信ブロック

## 2026-10-01

- 511fbe3: **W-08〜W-12 完了** — ENABLE_OLED オプション化、Core1 タイマハング解消、
  ホットキー厳密化、OLED スケルトン、アドバタイズ最適化
  フォルダ: https://drive.google.com/drive/folders/13xjsrNVW2sTLjl9vIhW8TW50rqYYwnwO
- fa29c9f: **W-01 / W-02 完了** — OLED LINE3 P1/P2 個別認識、LINE4 BTアイコン＋
  アドレス右寄せ
  フォルダ: https://drive.google.com/drive/folders/1QSABdh7SFNoZytR4Im35_jVc6oDLn4YX
- 67701d9: **W-04〜W-07 完了** — 非接続スロット切替キュー溢れ防止 (W-04)、
  サスペンド時切断タイムアウト (W-05)、Flash 書き込み競合防止 (W-06)、
  起動時 KBD 初期化順序保護 (W-07)
  フォルダ: https://drive.google.com/drive/folders/12DJVl9oCr4F-dq_uw15K78Z45exMknEB
- (時期不明・シート列当日付) 基盤整備: GATT Descriptor & Battery Service (NKRO マウス名
  PnP ID, Battery 100%)、TLV Flash Bank persistence (Slot & Bonding DB)、
  BLE Flow Control (CAN_SEND_NOW / ready-to-send gating / safe slot switch)、
  Hotkey stuck modifier fix (all-zero release report 転送)、Multicore Core 1 safe boot

## 2026-09

- 1290e78 (09-29): feat(ble) — マルチペアリングスロット管理とホットキー切替の導入
- ce0369f / da47736 (09-27): BLE HID support with Pico W (CYW43 + BTstack) と
  dual port 対応の初デコミット。以降が本フォーク独自の開発ライン
