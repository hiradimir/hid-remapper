# HISTORY.md

新しいセッションは AGENTS.md を読んだ後にこのファイルで経緯を追うこと。
新しい出来事は上に追記する。コミット完了時は TASKS.md の運用ルールに従い
仕掛中 → この履歴へ記録 → TASKS.md から削除する。

※ 本ファイルの初期内容は旧管理シート (xlsx「履歴」シート) の移植。
旧 ID 表記 (W-xx) は当時のもの。現在は BL-xx に統一。

---

## 2026-10-21 BL-39 実装: Wi-Fi UDP リモートログ (opt-in) + 名前付き UF2 保管運用の変更

- **BL-39 実装コミット `b50ad2e`** (ブランチ `bl39_remote_log`、`25a4bd7` から分岐):
  - 新規 `firmware/src/remote_log.{h,cc}`: 行単位リングバッファ (IRQ マスク付き spin lock、
    溢れ時 Drop-Oldest) → **メインループのみ**で Wi-Fi 非同期接続 + UDP フラッシュ +
    5s ALIVE ピング (`up= wifi= in= fwd= q= dr= dg= se=`)。リアルタイム経路 (BLE コールバック)
    からの追記は短いコピーのみ (不変条件 7 準拠)
  - **診断の勘所**: 沈黙時に ALIVE ピング停止=メインループ全体の死 / `in>0 fwd=0`=転送経路の死 /
    `fwd>0` で無応答=BLE 送信経路の死、と 3 分割できる
  - lwIP (NO_SYS) は `pico_cyw43_arch_lwip_threadsafe_background` への差し替えで同居
    (async_context 機構は none 版と共通のため BLE 伺服体制は不変)。`src/lwipopts.h` 追加
    (MEM_LIBC_MALLOC は threadsafe_background と非互換のため既定プール確保)
  - `ble_hid.cc` にフック約 20 点 (HCI 接続/切断 (reason)/暗号化/MTU/購読、切替・ペアリング・
    ボンドクリア、全タイマハンドラ)。いずれも `remote_log_eventf` 1 行追加で printf は不変
  - **opt-in 設計**: `-DENABLE_REMOTE_LOG=ON` かつ `src/remote_log_net.h` (gitignore・ルディ管理)
    がないと CMake が停止。OFF 構成は lwIP もリンクせず既存動作完全不変 (OFF/ON 両ビルド通過確認)
  - 受信ツール: `tools/udp_log_listener.py` (既定 udp/5140)
- **事故**: 基線再ビルド時、定石コマンド (`rm -rf build`) で build/ 直下保管の名前付き UF2 資料群
  (bl45 関連・基点版 11 本) を消失。**ルディ判断で roll-forward** (必要時はコミットから再ビルド。
  なお GATT DB Hash が当該環境ではランダム値のため再ビルドは機能同等・バイナリ非一致)。
  再発防止として**名前付き UF2 の保管先を `firmware/artifacts/` (gitignore・`git clean -df` 対象外) に変更**、
  build/ は作業用として消してよい運用に変更 (AGENTS.md ビルド節に追記)。
  現 HEAD の OFF 構成ビルドを `firmware/artifacts/remapper_bl39_off_b50ad2e.uf2` として保管
  (= A 版相当 (フロー制御込み) + BL-39 コード無効化分)。
- **進行中**: ルディが secret ファイル (`remote_log_net.h`) を自前作成 (エージェント非接触) →
  実機焼き → Wi-Fi 共存時の BLE 安定性確認 → 履歴へ

---

## 2026-10-20 BL-43/BL-45 調査: A 版フロー制御で半日長時間検証 → 「処理ループ沈黙 (ウェッジ)」の実観測

- **A 版** = `firmware/build/remapper_bl45_lockmsg_flow.uf2` (= HEAD `3933dba` (BL-04 完成品・スイッチロック・心拍ドット) + `btstack_config.h` BL-43 フロー制御ブロック)。**07:25 焼込み**。
- **Mac 側 bluetoothd ログによる遠隔観測** (Pico 側 UART 無しの代用)。証跡: `~/Desktop/bt_log_0725-1308_6h.log` (07:25〜13:08, 33MB)。
  - 07:25:22 接続 → **13時間44分 無切断**。この間 Mac ログの切断 45 回はすべて別デバイス **YutakaMBPR** (別 Mac のユニバーサルコントロール/Continuity 雷, F5:E8..., Mac=Peripheral role) のノイズで、**Remapper (E4:08:43:8B:24:34, cbuuid 1A59D2EC) の切断は 0 回**。朝の 4.44 秒切断パターンは消えた。
- **21:09 のスロット切替 (slot2) を境に悪化系列が始まった**:
  | 時刻 | 事象 | 解釈 |
  |---|---|---|
  | 21:09:01 | 719 (Pico発) 切断 | slot 切替による (= 正常動作) |
  | 21:26:03→43 | 再接続 → 暗号化開始で **734** | LL Response Timeout = Pico が暗号化手続きに 40 秒無応答 |
  | 21:45:57→21:46:05 | 再接続 → **暗号化成功・HID 完了 → 7.5 秒で 708** | Pico 側リンク層沈黙 (supervision timeout) |
  | 21:46 以降 | BLE 待機のまま、**USB 列挙・分類は生存** (VBUS 供給 / 入力デバイスの LED / 3 行目デバイス表示は更新される) のに **転送パイプライン・ホットキー・再広告が死んだまま** 15 分以上 Mac に繋がらない | 処理ループ品質のウェッジ |
  | 22:04 (電源再投入) | 接続 → 暗号化 → HID 完了 → **通電再投入で回復** | BL-43 既知の「電源再投入のみで復旧」と同型 |
  | 22:05:27 | 719 (Pico発) 切断 | **ルディの手動 slot 切替によるもの (= 正常)**。切替後 Windows (slot2) で正常動作を確認（転送・ホットキー復帰も含む） |
- **HCI reason コードの解釈** (Apple ログは 0x700 を加算した表記): 708=0x08 supervision timeout / 719=0x13 remote (Pico) terminated / 722=0x16 local host terminated / **734=0x22 LL Response Timeout (暗号化手続き未完了)** / 762=0x3E
- **BL-43 の問いへの回答**: 「現象発生時 OLED は表示更新を続けていたか?」→ **継続していた** (スロット状態・デバイス分類・`Switching...` 表示とも更新)。assert ハング系を否定し、**ロック/共有バス/ループ停滞系のウェッジ**であることを裏付け。
- **ボンド不一致は否定的**: 21:45 の接続は暗号化成功・HID まで OK (鍵は合っている)。Mac 側のペアリング削除・全消去・再ペアリングは不要と判断。
- **A 版フロー制御の評価**: 良かった点は「13h44m 連続稼働」まで伸びた (前の lockmsg 版は 30 分で 12 回の切断ループ)。ただし**スロット切替を起点とするウェッジ系列は残る** → 次の切り分け候補は **B (beeb02f: ble_lock カバレッジ拡大)** (x2 同梱: `remapper_bl45_x2_lock.uf2`) と **BL-39 (Wi-Fi UDP リモートログ: Pico 内部観測の手引き)**。
- **復帰後 (22:05 以降)**: 手動スロット切替 → Windows (slot2) ペアリング/転送とも正常。**22:04 の電源再投入でウェッジは完全回復**。ウェッジ状態中に見えた「Windows に繋がない」も同族現象だった可能性が高い。**注意 (BL-45 への含意)**: A 版 (x1 相当, フロー制御込み) でペアリング検出・接続が成立している → **フロー制御が「検出不可」を引き起こしていない**ことが実証され、BL-45 の残る原因は B か BL-31 系に絞られた。



- **経緯**: BL-04 初期修正 (4e76652 / f751492: 広告の強制開始・all-clear で pairing 再開) を
  実機で焼いたところ「ペアリングしても検出されない」「all-clear 後に検出不能」が悪化。
  切り分けのため 313dcd4 / b089ac1 / 6e4dc67 / 1c69e5f を順に焼いて確認し、
  **1c69e5f (ハウジング版で検出・接続 OK) を基準**に、必要な部分だけ再実装する方針に変更。
- **確認に使った UF2 (firmware/build/ 配下に保管・削除しない)**:
  - `remapper_before_bl43.uf2` (6e4dc67): Windows / macOS とも検出 OK
  - `remapper_at_1c69e5f.uf2`: 検出・接続 OK (**現在の基準**)
  - `remapper_before_oled_fix.uf2` (b089ac1): OLED は戻ったが BT 検出されず
  - `remapper.uf2` / `remapper_before_syphi.uf2` (BL-04 初期修正入り): 検出されず
- **最終コミット `b375cef`** (1c69e5f 派生 9 コミット、ブランチ `bl04_pairing_hotkey`):
  1. **all-clear**: 接続・保留キュー・送信フラグ・遷移/再接続タイマを全リセット。
     all-clear 後は自動で pairing を開始しない (ホットキー待ち)。
  2. **ペアリングはカレントスロットのみ対象**。別スロット指定では pairing を開始しない
     (遅延ペアリング方式は撤回し、関連コードは削除)。
  3. **ペアリングホットキーを Ctrl+Win+Alt+Space に変更、2段階化**:
     1回目 = カレントスロットのボンドを消去 (接続中なら切断) し、OS 表示を `-` に戻す /
     2回目 (消去済み) = そのスロットでペアリング開始。
     Space は Usage 0x2C → `report_data[6] & 0x01` (既存 Backspace/O と同じビット規則から算出)。
  4. **Ctrl+Win+Alt+F1〜F4 はスロット切替判定から外す** (Alt 付きは OS 側へ通す)。
     スロット切替は従来どおり Ctrl+Win+F1〜F4。
  5. **ペアリング中のカウントダウン復活**: `Pairing (60s)` → 秒ごとに `Pairing (Ns)` (ble_hid_task で秒変化時のみ描画)。
  6. **スロット切替・ON/OFF トグルの受付禁止 (3秒)** (`c821243`): `ble_switch_slot()` 入口で、受付から
     `SWITCH_LOCK_US` (3秒) 以内の要求を無視。BT 状態の不整合防止の保険。全消去・ボンド消去・ペアリングは対象外。
     禁止中は 4 行目に `Can't switch now` を表示し、明けたら通常表示へ戻す (`38c5666`)。
  7. **ペアリング開始時に `slot_transition_timer` を停止**する処理を復元 (`c821243`)。
     切替直後のペアリングで、保留中の切替タイマが状態を上書きするのを防ぐ。
  8. **接続中ハートビート (BL-34 の一部)** (`7b8d609`): BT 接続中のみ、1 行目のモードアイコン横で
     500ms ごとに `.` を点滅。USB フォールバック時は点滅しない（右上の電気アイコンで生存確認する運用）。
     1 行は 21 文字に収めるため空白を 1 つ減らした。
     出典は `wip/bl34-oled` (ec78a64) の `hb_char`。
- **実機確認 (ルディ)**: all-clear 後に勝手に pairing しない / slot2 でのペアリング開始 OK /
  Space の 2 段階 (消去 → `1:-*` 表示、再押下でペアリング) OK / カウントダウン OK / Windows でペアリング可 /
  切替直後の切替禁止 OK・3秒後の切替 OK / 点滅ドット OK / 禁止中の `Can't switch now` 表示 OK。
- **コミット一覧** (`bl04_pairing_hotkey`, 基準 1c69e5f): `708de74` `4daaf18` `ceb75a0` `84ee25d` `c4d5123`
  `1e74763` `b3a7f13` `4d2cdee` `b375cef` `c4c6d9f` (docs) `c821243` `7b8d609` `38c5666`。
  途中の `all-clear 再開` / `遅延ペアリング` は最終的に撤回・削除済み。
- **検証用 UF2 (firmware/build/)**: `remapper_hotkey_space_candidate.uf2` (b375cef 相当),
  `remapper_switchlock_heartbeat_candidate.uf2` (7b8d609 相当), `remapper_lockmsg_candidate.uf2` (38c5666)。
- **BL-34 の残り**: スクリーンセーバー・Toast・バウンスロゴは `wip/bl34-oled` 側のまま。取り込みは別途判断。
- **未解決・注意**:
  - Windows 接続時に OS 判定が `A` になる (macOS は `M` で正常) → BL-44
  - b089ac1 以降で検出されなくなった原因は未特定 (6e4dc67 は OK) → BL-45 (BL-43 系マージ区間)
  - `feature/pico-w-ble` には初期修正の revert (1cee7c5, 2fb1e7e) が積まれており、本ブランチとは別線。
    統合方針はルディ判断 (現時点では未マージ)
  - 作業開始時に HEAD が `1c69e5f` の detached 状態だったため、コミットを `bl04_pairing_hotkey` ブランチに固定した

## 2026-10-18 (後半・シルフィ並行作業)

- **BL-43 根本原因調査を開始** (ルディ指摘: 旧メモは復旧手法のみで cause 掃討欠落。仰る通り):
  - コード監査 + pico-sdk 2.1.1 / BTstack 1.6.2 (submodule 固定) の CYW43439 共有バス
    (cybt_shared_bus) 実装を精読。**根本原因仮説 (有力度順)**:
    1. **cyw43 共有バス オーバーラン設定欠落 (最有力)** — 上流 pico-examples の
       btstack_config.h は `MAX_NR_CONTROLLER_ACL_BUFFERS 3` +
       `ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL` (`HCI_HOST_ACL_PACKET_LEN 1024 /
       NUM 3`) を「cyw43 shared bus overrun 回避」のために必須設定としており、本プロジェクトの
       btstack_config.h はこれが**一つも無い**。HCI_ACL_PAYLOAD_SIZE 1691+4 のまま。
       接続/切断サイクルと高頻度 NTF が積み重なるとコントローラ飛行バッファ破綻 →
       HCI イベント evaporate →silent 化。既知の再現事例がまさに同症状
       (pico-sdk #2181 / btstack #654: 「6〜8 回の切断/再接続でスタックが沈黙・電源投入のみ復旧」…
       報告者は ACL パケットサイズ縮小で解消とコメント)。本件の「長時間運用で回数を重ねると発症」
       と整合する
    2. **ble_lock 適用が send 経路のみ (BL-33 ロックの穴)** — async_context
       (threadsafe_background で低優先 IRQ 実行) に対し、main loop 側で btstack API を叩く
       ble_switch_slot / ble_enter_pairing_mode / ble_clear_bonds / ble_hid_check_hotkey /
       ble_hid_kick_reconnect / ble_hid_task / TLV 保存 (conn_stabilize 由来含む) は
       **非ロック**。長時間でのレア競合がスタック状態破壊 → 電源再投入のみ復旧のパターン候補
    3. **cybt_shared_bus の assert ハング** — `cybt_bus_request` は wake 失敗時に
       `assert(err==0)`、読み出し失敗にも assert (NDEBUG 未定義)。発症時点でボード全体が
       凍結するはず (OLED も停止)。ルディの観測で除外 or 確定させる
    4. RF・電源 (PFM, 2.4GHz 干渉) — 旧メモの対処療法系。SDI とは別の外因として最後まで残す
  - 補足: BL-17/BL-23 の保護 (2500ms クールダウン・30s 再接続タイムアウト) は pairing ストーム用で
    RECONNECTING 連鎖に対する閉塞カウントは無く、R1 発症時に無限サイクルになる — 保護追加は
    対処であって原因ではない (R1 直すまで凍結)
  - **実施計画** (P1 はデバイス不要・build 検証可): テストブランチで (a) 上流準拠の
    フロー制御設定投入 → (b) ble_lock 穴の全閉包 → (c) 切断 reason カウンタ (OLED 内・UART 不要)
    の観測面追加。実機復帰後 (d) ハウジングソークテストで切り分け
- ブランチ体制を整備し、実機検証できない間のビルド検証型タスクを並行処理した:
  - `wip/bl34-oled` (ec78a64): BL-34 パッチ+修正 4 ファイルを WIP コミットで退避。
    実機検証 OK 後に正式化 (stash は空・feature/pico-w-ble に戻す構成)
  - `feature/pico-w-ble` (c5417d6): md 5 点 (AGENTS/PROJECT/TASKS/HISTORY/CHARACTER) を
    docs コミット。sample-agents/ は untracked 保持 (参照のみルール)
  - `bl20_compile_warnings` (c3c1525): **BL-20 完了** — 再現 5 件
    (out_report.cc -Wsign-compare 1 + remapper.cc -Wnarrowing/-Wsign-compare 4) を潰し、
    BLE-ON/OFF・OLED-ON/OFF・single-host・dual_b 各構成で warning 0 を確認。
    BL-20 が挙げていた main.cc / oled.cc の警告は現行推しで再現せず (過去コミットで解消済み)
  - `bl40_usb_detect_fast` (df59cd2 + bl20 マージ 978aba2): **BL-40 実装完了** —
    oled_update_usb_ports() 周期 5s→500ms、`oled_request_usb_ports_refresh()` 追加、
    device_connected/disconnected_callback から即時再判定要求。stub ビルド warning 0
  - 注意: bl40 ブランチは bl20 を取り込んでいるため、**bl20 単独マージ時は
    bl20_compile_warnings を feature/pico-w-ble にマージしてから bl40 を重ねるのが推奨**
- 参考情報: ルディのホスト (Mac) に Arm GNU Toolchain 15.3 があり、devcontainer 無しでも
  cmake ビルド可能と判明 (検証系の高速化に使う)
- 待ち事項: BL-34 実機検証 / BL-40 表示遅延確認 / BL-20・BL-40 のマージ先決定 (ルディ)

## 2026-10-18 (前半・初期化セッション)

- **cli セッション 1 初 days (シルフィ・opencode)**: 運用を cli エージェント (シルフィ) セッションへ移行。
  旧管理シート xlsx「HID Remapper BLE - 開発管理シート」を sample-agents/ に置き、
  内容を **AGENTS.md / PROJECT.md / TASKS.md / HISTORY.md / CHARACTER.md** へ移植した
  (→ 後半で docs コミット c5417d6 として固定)。以後のタスク台帳・履歴の主はこの *.md 群。
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
