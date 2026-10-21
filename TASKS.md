# TASKS.md

旧「HID Remapper BLE - 開発管理シート.xlsx」(sample-agents/ 配下に原版あり) の
「バックログ」「仕掛中」「Agentへの指示」シートを移植したもの。
**新しい出来事は HISTORY.md に記録**し、このファイルはタスク台帳として運用する。

## 運用ルール (xlsx「Agentへの指示」シートからの移植)

- ID 体系: 全て **BL-xx** に統一 (旧 W-xx は廃止済み・履歴参照は許容)
- **バックログ** = 未着手段のタスク一覧 (ステータス管理不要、存在＝未着手)
- **仕掛中** = 現在作業・パッチ作成・検証中のタスク (存在＝対応中)
- **コミット完了時**: ルディからコミット報告があったら、仕掛中のタスクを
  **HISTORY.md (履歴) に記録**し、仕掛中をクリア、バックログからも削除する
- 検証証跡 (Google Drive フォルダ) はルディ管理。リンクは HISTORY.md に記録する

---

## 仕掛中

| ID | タスク名 | 状態 | 詳細・進捗 |
|----|----------|------|------------|

| BL-34 | OLED表示改善・低輝度スクリーンセーバー機能 | 実装完了・**実機検証中** (パッチ修正済) | 旧 agent パッチ (3段階減光 0x8F→0x30→0x05 + バウンスロゴ, Toast, ペアリング60s/再接続30s カウントダウン) を評価・修正済: Line3 の21文字超え欠け直し、extern "C" 宣言廃止→ble_hid.h include、reconnect_start_us 未適用経路補完。加えて Line1 の slot 状態行は「slot4 文字はみ出し」を回避するためスペース調整 (OLED 固定表示幅21文字想定) を反映。BLE-ON/OFF 両ビルド通過 (warning 0)。ハウジング版で動作確認済 → soak 待ち（切断ループ等の別不具合は適宜調整） |
| BL-01 | 再起動時の BT-OFF (スタンバイ) 状態の Flash 保持 | 検証中 (286c1b3コミット済) | スロットトグルOFF状態で再起動した際、自動でアドバタイズ開始せず STANDBY_SILENT を維持 |

---

## バックログ

| ID | タスク名 | 優先度 | カテゴリ | 詳細・仕様メモ |
|----|----------|--------|----------|----------------|
| BL-03 | Web Configurator との連携 | 低 | WebUI | スロット名・OS 手動設定の反映 |
| BL-19 | GAP Device Name の動的化 (スロット別表示名) | 中 | BLE機能 | GATT 0x2A00 を動的化し Remapper [Slot X] としてホストに登録 |
| BL-20 | コンパイル警告の解消 **[実装完了・要マージ判断]** | 低 | コード品質 | 再現 5 件 (out_report.cc 1 + remapper.cc 4) 解消済み → ブランチ `bl20_compile_warnings` (c3c1525)。BLE-ON/OFF・OLED 条件・single-host・dual_b 各構成で warning 0 検証済。main.cc/oled.cc の記載警告は過去コミットで解消済みを確認 |
| BL-26 | USBモード動作時のキーボード操作引っかかり調査 | 低 | USB/キーボード | usb モードで動いている時の引っかかり (再現不定期)。再現頻度・バッファ・SPI割り込み影響確認 |
| BL-31 | RP2350-W (Pico 2 W / Waveshare Type-C版) 対応 **[方針確定〜移行開始]** | 中 | ハードウェア・移植 | RP2350B-Plus-W Type-C 実機情報から `src/boards/waveshare_rp2350_plus_w.h` を追加し、CMake 側で ENABLE_BLE を RP2350 系ボードでも許可。ビルド (PICO_BOARD=waveshare_rp2350_plus_w, BLE=ON/OLED=ON) は通過。USB host は pico-w/pico2-w と同一 DP/D- マッピング (GPIO 0/1) をデフォルトにし、実機検証でズレがあれば `PICO_DEFAULT_PIO_USB_DP_PIN` 上書きで調整 |
| BL-36 | スロット切替遷移中のマウス入力遅延 (1秒)・キーボード入力不全調査 | 中 | BLE通信安定性 | 切替中 (未接続〜再接続中) のマウス連続操作で切替後に約1秒遅延・キーボード入力不全 (UARTログなし)。レポートバッファ滞留、Drop-Oldest、接続直後の can_send_now スタールを調査 |
| BL-37 | OLED 表示乱れ・更新ムラ解消 (I2C 100kHz化) および Flash書き込み時 Core 1 ハング防止 | 低 | HW / マルチコア安定性 | ハウジング組み込みで配線短縮済み。安定稼働中のため再現待ち |
| BL-38 | Windows 環境の BLE 接続性・再接続性改善 | 低 | 改善 | 再現せず (Flash内旧ボンドゴミ起因の可能性大)。OLED 可視化 C:E_ K_ M_ を残して監視 |
| BL-40 | USB ホストのデバイス種別判定の即時化・周期短縮 **[実装完了・要実機確認]** | 低 | コード品質 / USB | oled_update_usb_ports() 周期 5s→500ms + `oled_request_usb_ports_refresh()` (接続/切断コールバックから即時再判定要求) → ブランチ `bl40_usb_detect_fast` (df59cd2, bl20 マージ込み)。実機の表示ラグ確認待ち |
| BL-41 | OS 連動 Modifiers (Cmd ⇔ Ctrl) 自動スワップ | 中 | キーマップ / BLE | スロットの OS 判定結果 (W, A, M, I) に基づき、Windows/Android 接続時は Cmd(GUI) と Ctrl を自動スワップ |
| BL-42 | モバイルバッテリー運用時の Keep-Alive パルス制御・Quiet Mode | 低 | HW / 電源 | キーボード抜去時のバッテリー自動給電停止・軽負荷バーストでのピエゾ鳴き対策。空き GPIO (GP6 等) FET+47Ω パルス疑似負荷、または CYW43 WL_GPIO1 (Quiet Mode) 検証 |

| BL-43 | ハウジング版長時間稼働時の断続的 BT 切断ループ・CYW43 ストール調査と復旧ロジック **[P1 対策実装済み・実機 soak test 待ち (ブランチ `bl43_bt_root_cause`)]** | **高** | BLE安定性 / HW | 手動トグルでは改善せず電源再投入でのみ復旧 (切断ループ)。想定原因 ③ (cybt_shared_bus assert ハング) はルディ観察 (OLED 更新継続) により除外済み。P1 対策をコミット済: (1) btstack_config.h に上流準拠の controller ACL バッファ上限 3 + HCI Controller-to-Host フロー制御 (pico-sdk #2181 / btstack #654 準拠), (2) メインループ側 BTstack API 入口 (switch_slot / pairing_mode / clear_bonds / task / kick_reconnect) への ble_lock 適用 (async context ロックは recursive_mutex でネスト安全)。soak test で切断ループ再発が消えるか確認 → OK なら feature へマージ |
| BL-44 | Windows 接続時の OS 自動判定が "A" になる | 中 | BLE機能 | 2026-10-19 実機: Windows でペアリング・接続は可能だが OS 表示が A (macOS は M で正常)。ATT MTU / Connection Update の判定条件 (peer_has_android_timeout・ble_conn_timeout>=450・mtu>=500) を Windows の値で再確認 |
| BL-45 | BL-43 系マージ区間の BT 検出不可の切り分け (bisect) | **高** | BLE安定性 | 実機 (ハウジング版) 確認: 6e4dc67 は検出OK / 1c69e5f は検出・接続OK / b089ac1 (OLED 修正直前) は検出されず。差分の bisect は未実施。現在の確認済みベースは 1c69e5f (ブランチ bl04_pairing_hotkey)。**2026-10-20**: 切り分け用 UF2 2 本作成済み (`remapper_bl45_x1_flowctl.uf2` = 1c69e5f+フロー制御 / `remapper_bl45_x2_lock.uf2` = 1c69e5f+ble_lock 拡大)。A 版 (HEAD+フロー制御) の観察結果は HISTORY 2026-10-20 参照 |
| BL-46 | Pico W BLE キャリア基板の設計〜JLCPCB 発注 (ハウジング基板化) | 低 | ハードウェア / 基板設計 | **2026-10-20 中断・記録のみ** (会話で示した案の要点)。・リポジトリ `custom-boards/` に上流 (jfedor2) 由来 KiCad プロジェクト v1〜v8 あり (Type-C/Type-A フットプリント・JLC 用製造 CSV の書式を流用可) が、**v1〜v8 はベア RP2040 構成 (無線なし) のためそのままでは BLE フォーク不可**。→ Pico W モジュールを載せるキャリア基板を新設が現実解。・**配線要件**: USB-A ×2 (ホスト1: D+=GP0/D-=GP1, ホスト2: D+=GP2/D-=GP3 — `remapper_single.cc` の `PICO_DEFAULT_PIO_USB_DP_PIN=0` / `PICO_DUAL_PIO_USB_DP_PIN=2` と一致, VBUS は Pico pin40 から供給), OLED 4p (I2C0 SDA=GP4/SCL=GP5), USB-C は**電源専用** (CC1/CC2 に 5.1kΩ×2 プルダウン必須。VSYS への給電は SS14 ショットキー 1 個で PC USB (Pico 直) と競合回避)。・**PC データ接続**: 標準 Pico W はヘッダに USB D+/D- 非露出。代替 (1) Pico 自前 micro-USB をハウジング穴から出す (推奨・追加はんだなし), (2) **モジュール裏面テストパッド TP2=USB DM / TP3=USB DP** から細線結線で基板 USB-C へデータ接続 (手付け 2 点で可。将来拡張用に基板側に結線パッドを Reserve する案)。・**RP2350/RP2040 チップ直付け (無線込みの専用実装)** は CYW43 系無線モジュール調達・アンテナ設計の負担大で非推奨。・ツール: KiCad での回路図→レイアウト→DRC→ガーバー/BOM/CPL 作成は可能。**未決事項**: 外形寸法・ポート位置 (ハウジングから頂く), OLED コネクタピッチ (現物確認), Pico W 直付け or ソケット, TP2/TP3 結線の採否, 置き場所 (例: `custom-boards/pico-w-ble/`) |
| BL-47 | Wi-Fi 設定 (SSID/パス) の非焼き込み供給経路 **[v2 課題]** | 中 | Wi-Fi / 設定 | BL-39 は `src/remote_log_net.h` (gitignore) に焼き込む運用。secret をソース/UF2 に残さない供給経路を用意する — (a) Web configurator (WebHID) から Flash に保存, (b) 接続済みキーボードのホットキーモードで SSID/パス入力 → Flash 保存 (OLED は入力状態の表示担当)。実装時は既存の config 保存領域と Flash 書き込み時の Core 1 停止 (BL-37 不変条件) に従う |
