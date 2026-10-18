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
| BL-34 | OLED表示改善・低輝度スクリーンセーバー機能 | 実装完了・**実機検証待ち** (パッチ修正済) | 旧 agent パッチ (3段階減光 0x8F→0x30→0x05 + バウンスロゴ, Toast, ペアリング60s/再接続30s カウントダウン) を評価・修正済: Line3 の21文字超え欠け直し、extern "C" 宣言廃止→ble_hid.h include、reconnect_start_us 未適用経路補完。BLE-ON/OFF 両ビルド通過 (warning 0)。作業ツリーにあり未コミット・stash 空。実機でスクリーンセーバー遷移・ダウンだれ (BL-37 常在要素の切分け) を確認 → OK ならコミット |
| BL-01 | 再起動時の BT-OFF (スタンバイ) 状態の Flash 保持 | 検証中 (286c1b3コミット済) | スロットトグルOFF状態で再起動した際、自動でアドバタイズ開始せず STANDBY_SILENT を維持 |
| BL-04 | 接続時 OS 自動判定 (フィンガープリント) およびスロット保持 | 検証中 (286c1b3コミット済) | ATT MTU / Connection Update による自動判定、Stickyガード、Cmd+Ctrl+Alt+O 手動切替、2秒遅延デバウンスFlash保存。Mac/Android実機確認済、Win検証待ち |

---

## バックログ

| ID | タスク名 | 優先度 | カテゴリ | 詳細・仕様メモ |
|----|----------|--------|----------|----------------|
| BL-03 | Web Configurator との連携 | 低 | WebUI | スロット名・OS 手動設定の反映 |
| BL-19 | GAP Device Name の動的化 (スロット別表示名) | 中 | BLE機能 | GATT 0x2A00 を動的化し Remapper [Slot X] としてホストに登録 |
| BL-20 | コンパイル警告の解消 **[実装完了・要マージ判断]** | 低 | コード品質 | 再現 5 件 (out_report.cc 1 + remapper.cc 4) 解消済み → ブランチ `bl20_compile_warnings` (c3c1525)。BLE-ON/OFF・OLED 条件・single-host・dual_b 各構成で warning 0 検証済。main.cc/oled.cc の記載警告は過去コミットで解消済みを確認 |
| BL-26 | USBモード動作時のキーボード操作引っかかり調査 | 低 | USB/キーボード | usb モードで動いている時の引っかかり (再現不定期)。再現頻度・バッファ・SPI割り込み影響確認 |
| BL-31 | RP2350-W (Pico 2 W / Waveshare Type-C版) 対応 | 中 | ハードウェア・移植 | RP2350 アーキテクチャ (Cortex-M33)、CYW43439 BLEスタック、Pico-PIO-USB の RP2350 動作検証、Flash (TLV) 保存領域の適合 |
| BL-36 | スロット切替遷移中のマウス入力遅延 (1秒)・キーボード入力不全調査 | 中 | BLE通信安定性 | 切替中 (未接続〜再接続中) のマウス連続操作で切替後に約1秒遅延・キーボード入力不全 (UARTログなし)。レポートバッファ滞留、Drop-Oldest、接続直後の can_send_now スタールを調査 |
| BL-37 | OLED 表示乱れ・更新ムラ解消 (I2C 100kHz化) および Flash書き込み時 Core 1 ハング防止 | 低 | HW / マルチコア安定性 | ハウジング組み込みで配線短縮済み。安定稼働中のため再現待ち |
| BL-38 | Windows 環境の BLE 接続性・再接続性改善 | 低 | 改善 | 再現せず (Flash内旧ボンドゴミ起因の可能性大)。OLED 可視化 C:E_ K_ M_ を残して監視 |
| BL-39 | Wi-Fi (UDP / Syslog) 経由のリモートログ送信 | 中 | デバッグ / 通信 | ハウジング運用 (UART未接続) 時のリアルタイムログ収集。BLE HID への影響最小化のため重要イベント送信・バッファリング等の共存制御を考慮 |
| BL-40 | USB ホストのデバイス種別判定の即時化・周期短縮 **[実装完了・要実機確認]** | 低 | コード品質 / USB | oled_update_usb_ports() 周期 5s→500ms + `oled_request_usb_ports_refresh()` (接続/切断コールバックから即時再判定要求) → ブランチ `bl40_usb_detect_fast` (df59cd2, bl20 マージ込み)。実機の表示ラグ確認待ち |
| BL-41 | OS 連動 Modifiers (Cmd ⇔ Ctrl) 自動スワップ | 中 | キーマップ / BLE | スロットの OS 判定結果 (W, A, M, I) に基づき、Windows/Android 接続時は Cmd(GUI) と Ctrl を自動スワップ |
| BL-42 | モバイルバッテリー運用時の Keep-Alive パルス制御・Quiet Mode | 低 | HW / 電源 | キーボード抜去時のバッテリー自動給電停止・軽負荷バーストでのピエゾ鳴き対策。空き GPIO (GP6 等) FET+47Ω パルス疑似負荷、または CYW43 WL_GPIO1 (Quiet Mode) 検証 |
| BL-43 | ハウジング版長時間稼働時の断続的 BT 切断ループ・CYW43 ストール調査と復旧ロジック **[P1 対策実装済み・実機 soak test 待ち (ブランチ `bl43_bt_root_cause`)]** | **高** | BLE安定性 / HW | 手動トグルでは改善せず電源再投入でのみ復旧 (切断ループ)。想定原因 ③ (cybt_shared_bus assert ハング) はルディ観察 (OLED 更新継続) により除外済み。P1 対策をコミット済: (1) btstack_config.h に上流準拠の controller ACL バッファ上限 3 + HCI Controller-to-Host フロー制御 (pico-sdk #2181 / btstack #654 準拠), (2) メインループ側 BTstack API 入口 (switch_slot / pairing_mode / clear_bonds / task / kick_reconnect) への ble_lock 適用 (async context ロックは recursive_mutex でネスト安全)。soak test で切断ループ再発が消えるか確認 → OK なら feature へマージ |
