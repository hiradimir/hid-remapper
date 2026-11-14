# HISTORY.md

新しいセッションは AGENTS.md を読んだ後にこのファイルで経緯を追うこと。
新しい出来事は上に追記する。コミット完了時は TASKS.md の運用ルールに従い
仕掛中 → この履歴へ記録 → TASKS.md から削除する。

※ 本ファイルの初期内容は旧管理シート (xlsx「履歴」シート) の移植。
旧 ID 表記 (W-xx) は当時のもの。現在は BL-xx に統一。

---

## 2026-11-14 `wake71`: 送信失敗時にレポートを捨てない（キー固着の防止）

ルディの観察: `wake70` で「キーボードがたまに効かない。効いた時も入力が固定される
（key-up が出ていない）」→ **送信キューが溢れたときの挙動**が原因。

```c
sent = do_send_report(...);
// XXX even if not sent?
or_head = (or_head + 1) % OR_BUFSIZE;   // ← 送れなくても pop していた
or_items--;
```

キーボード/マウスのレポートは「最新が全状態を持つ」ので、古いものが落ちるのは害が無い。
しかし**最新（key-up やボタン離し）が送れずに捨てられると、ホスト側で押しっぱなし**になる。

**修正**: 送れなかったらキューに残して次のスロットで再試行する。

```c
if (!sent) return false;
```

これで過負荷で溢れても「古いものが Drop-Oldest で落ち、最新は必ず送られる」動作になる。

- ビルド: `firmware/artifacts/remapper_anchor_wake71_flood3.uf2`（11/14 22:35,
  `REPRO_FLOOD_MS=15` で過負荷テスト込み）
- 検証: ①通常使用で引っかからない ②キーを押したまま BT を切って戻しても
  押しっぱなしにならない ③flood 中でも入力が固定されない

## 2026-11-14 `wake70`: 再現テストを「送信キュー経由」に修正（過負荷の忠実化）

`wake69` のフラッドは `ble_hid_send_report()` を直接叩いていた。この実装だと
**許可待ちで自分自身が `blk` に計上される**ため、「過負荷」ではなく
「壊れた送信者」になっていた。実測で `in=0` でも `blk` が 26/s 増え続け、
`lat` が 36〜69ms に固定された（ルディの「負荷が下がっても戻らない」の正体）。

**修正**: ゼロ移動のマウスレポートを **remapper の送信キューに積む**方式へ変更。

- キューが空のときだけ積む（自分でキューを埋めない）
- 通常のペーシング経路で送られるので、`blk` は本物の入力と競合したときだけ増える
- 実装: `outgoing_reports[or_tail]` に report_id=1 のゼロレポートを書き、`or_tail`/
  `or_items` を進める（`or_tail` を extern 追加、宣言を前方へ移動）

- ビルド: `firmware/artifacts/remapper_anchor_wake70_flood2.uf2`（11/14 22:05）

## 2026-11-14 `wake69`: 再現テストを作り直し（送信し続ける版 `REPRO_FLOOD_MS`）

ルディの指摘で**モデルが間違っていた**:

> スロットのスイッチのタイミングじゃなくて。ずっと使ってて何故か切れて、
> 切れてると知らずに入力してて、その後膠着みたいなイメージ

つまり本質は「**送信し続けている最中に、静かに死ぬ**」。`wake68` の
「切断⇄再広告を繰り返す」は見当違いだった。

**作り直し**: 一定周期で**ゼロ移動のマウスレポートを送り続ける**（カーソルは動かないが
ACL 送信は実際に発生する）。これで「ずっと使っている」状態を人手なしで合成できる。

- `-DREPRO_FLOOD_MS=15` で 15ms ごと（＝実運用の最大レートと同じ 66/s）
- 膠着すれば既存の検出系（`ADV-NOACK` → `DEAD-SNAP` → `HCI-RECYCLE`）が動く
- **実験専用**（常時送信するのでホストを起こし続ける）

- ビルド: `firmware/artifacts/remapper_anchor_wake69_flood.uf2`（11/14 16:45）

## 2026-11-14 `wake68`: 膠着の再現テスト（`REPRO_CYCLE_MS`）

「たまに起きる」を追い続けるのは非効率なので、**狙って起こす**手段を用意した。
過去の膠着に共通するのは「**接続直後に送信が始まる**」場面（再接続 2 秒後、
スロット切替 4 秒後など）。接続/切断のたびに HCI コマンドが連続して送られる状態を
一定周期で作り出す。

- `-DREPRO_CYCLE_MS=10000` で 10 秒ごとに「切断 ⇄ 再広告」を自動で繰り返す
- ログ: `BT REPRO n=.. disc up=..` / `BT REPRO n=.. re-adv up=..`
- 膠着すれば既存の検出系（`ADV-NOACK` → `DEAD-SNAP` → `HCI-RECYCLE`）が動く
- **実運用には使えない**（勝手に切断する）。机の上での再現実験専用

**使い方**: 焼いて放置 → `REPRO` の回数と `ADV-NOACK` / `ccmd=0` の有無を見る。
再現したら、その状態で `hci_dump`（UART）を取れば「どのコマンドの応答が落ちたか」が
特定できる可能性がある（最後の切り札）。

- ビルド: `firmware/artifacts/remapper_anchor_wake68_repro.uf2`（11/14 16:35）

## 2026-11-14 `wake67`: 再接続しても USB-OUT のままになる循環を断つ

**症状**: 11/14 10:58 に膠着 → `PROACT` → 3 秒後に再接続（`CONNECTED slot=1`）したのに、
OLED は `USB-OUT` のまま。BLE は健全（`ccmd=1`）、`ok` は凍結、`fl=0x3F`（`tx_stalled`）。

**原因**: `wake49` で入れた `health_tx_stalled = true`（CANSEND-TO 時）が
**接続成立では解除されない**。解除条件は「送信成功」だけだが、`tx_stalled` が立っていると
転送先が USB に固定され、**BLE では一度も送信しない** → `ok` が進まない →
永久に解除されない、という循環。

```
tx_stalled=true → USB に流す → BLE で送らない → ok 進まない → 解除されない
```

**修正**: 接続成立（`connected_now && !was_connected`）で `health_tx_stalled = false`。
新しいリンクなので一度は試す。それでも詰まれば再び `CANSEND-TO` で立つ。

- ビルド: `firmware/artifacts/remapper_anchor_wake67_soak.uf2`（11/14 11:00）

## 2026-11-13 `wake66`: 観測周期を HB 5 秒 / ALIVE 30 秒に振り替え

ルディの提案。**`BLE-HB` は膠着の前後を細かく見たい行**なのに 15 秒間隔で、
`ALIVE`（RSSI・seq/ack など緩やかな指標）が 5 秒間隔だった。優先度が逆。

- `RLOG_BLE_HB_MS` … 15000 → **5000**
- `RLOG_ALIVE_MS` … 5000（ハードコード）→ **30000**（マクロ化して調整可能に）

通信量はほぼ同じ（HB は長いが本数が増えるぶん、ALIVE を減らして相殺）。

- ビルド: `firmware/artifacts/remapper_anchor_wake66_soak.uf2`（11/13 23:16）
- 構成は `wake65` と同じ（衝突テスト・ストレスなし / `ccmd` 観測 / 自動復帰 /
  `LAT-HIGH` 無効 / BL-43 OFF / `bMaxPower=500`）

### 2026-11-13 のその他の記録

- **フラッシュ後の USB ホスト取りこぼし**: `wake65` を焼いた直後、キーボード
  (04D8:EA54) が列挙されず（SlimBlade は列挙された）、挿し直しで復帰。
  起動 16 秒後に `USB MNT dev=2` が出ている。焼いた後は**入力デバイスも確認**する
- SlimBlade の内蔵スピーカーが BT ON/OFF のたびにポップする（ON で 3 回）。
  USB は外れないのでブラウンアウトではない。IRQ 停止では鳴り方は変わらない

## 2026-11-13 `wake65`: 常用（soak）ビルドに戻す + USB 消費電流申告を 500mA に

**実験の結論**: 衝突テスト（`CMD_COLLIDE_US`）を 20ms → 60ms に延ばし、
切断コマンドにも適用したが **`ccmd=0` は再現しなかった**。
→ 「IRQ/バス停止でコマンド応答を取りこぼす」説は**否定方向**。

さらにログを読み直すと順序が逆だった:

```
22:24:28  CANSEND-TO hci_idle=2009ms   ← この時点で既に HCI イベントが 2 秒来ていない
22:24:28  PROACT → 切断コマンド送信
22:24:31  ccmd=0                       ← 応答が来ないのは「結果」
```

**クレジット枯渇は結果**で、その前にチップが既に沈黙していた。
つまり原因は「**チップの BT 側が内部要因で黙る**」で、BTstack / CYW43 ドライバからは
見えない領域。外から制御する手段が無い。

**否定できた仮説（すべて実測）**: メインループ停止 (`lmax` 4ms) / IRQ 停止 (60ms でも
再現せず) / Flash 書き込み / Wi-Fi 共存 / 接続回数 / リーク / 熱 / 電波 /
コマンド拒否（`ADV-* status` は全ログ 0x00 のみ）。

**対応**: 原因究明はここで打ち止めにし、**復旧の速さで運用**する。

- `wake65` = soak 用（衝突テスト・ストレス無し）
  - `CCmd` 観測（HB の `ccmd=`）、`DEAD-SNAP`、`HCI-RECYCLE`、自動再起動は維持
  - `LAT-HIGH` 無効、BL-43 フロー制御 OFF（`wake54`/`wake52` と同じ）
  - **USB の `bMaxPower` を 100 → 500 mA に修正**（2 台に給電している実態に合わせる）
- ビルド: `firmware/artifacts/remapper_anchor_wake65_soak.uf2`（11/13 23:10）

### 補足: SlimBlade のスピーカーポップ

BT ON/OFF のたびに SlimBlade の内蔵スピーカーがポップする（ON で 3 回）。
USB は外れない（`USB MNT` の再発行なし）のでブラウンアウトではない。
IRQ 停止をしても鳴り方は変わらない（＝こちらの停止が原因ではない）。
**無線送信に同期した小さな電気的過渡**で、膠着との因果は証明できていない。

## 2026-11-13 `wake64`: 衝突テストを 60ms に延長 + 切断コマンドにも適用

`wake62`（20ms、広告コマンドのみ）では **BT ON/OFF を繰り返しても `ccmd=0` が出なかった**。
チップのイベントキューが 20ms 分では溢れないと判断し、以下を変更:

- 停止時間を **60ms** に延長（`CMD_COLLIDE_US=60000`）
- **PROACT の切断コマンド直後**にも同じ停止を挿入
  （実機で応答を落としたのが `HCI_Disconnect` だったため）

### 補足: エラーコード確認について

ルディの質問（Google AI 記事のチェックポイント「エラーコードの確認」）に対して:

- 広告系コマンドの `status` は**全部ログに出している**（`ADV-ADDR` / `ADV-PARAM` / `ADV-EN`）
- **全ログで 0x00 以外は 0 件**。`0x01` / `0x0C` / `0x12` は一度も出ていない
- 今回の膠着は「コマンドが**拒否**された」のではなく「**応答が来ない**」なので、
  そもそもエラーコードが存在しない。`ccmd`（クレジット残量）でしか検知できない

- ビルド: `firmware/artifacts/remapper_anchor_wake64_collide60.uf2`（11/13 23:02）

## 2026-11-13 `wake63`: ログ 1 行の上限を 384 に拡大（HB の末尾が切れていた）

ルディが「`ccmd` で切れているログがある」と気づいた。原因は **`RLOG_LINE_MAX 256`**。
HB 行が `ccmd=` を足したことで **257 文字**になり、末尾 2 文字（`=1`）が落ちていた。

- `RLOG_LINE_MAX` を **384** に拡大（データグラム上限 512 - プレフィックス 16 = 496 に収まる）
- これで `HB … ccmd=` も `DEAD-SNAP … ccmd=` も完全に記録される

**教訓**: 観測項目を足すたびに 1 行が長くなる。**行長上限を意識しないと、
一番欲しい末尾の値が静かに落ちる**（今回まさに `ccmd` が落ちていた）。

- ビルド: `firmware/artifacts/remapper_anchor_wake63_collide.uf2`
  （11/13 23:05、`CMD_COLLIDE_US=20000` の衝突テスト込み）

## 2026-11-13 `wake62`: コマンド衝突テスト（狙い撃ちの再現実験）

ルディの指摘「周期タイマーの Flash 書き込みでは**衝突を狙えない**。BT の操作は
人間由来でランダムだから」は正しい。`wake61` の周期ストレスは確率が低すぎる。

**狙い撃ち**: 広告有効化コマンドを送った**直後に IRQ を 20ms 止める**。
「コマンドの応答が返る瞬間にホストが止まる」状況を確実に作れる。

```
gap_advertisements_enable(enable);
→ save_and_disable_interrupts(); 20ms 待つ; restore_interrupts();
→ BT CMD-COLLIDE us=20000 en=1
```

これで膠着（`ADV-NOACK` → `ccmd=0`）が出れば、**機序が完全に確定**する。

### 同時に入れたもの

- `STALL_EMU_MS` / `STALL_EMU_US` … 周期的に IRQ を止める（Flash 摩耗なしの汎用ストレス）
- `CMD_COLLIDE_US` … 上記の狙い撃ちテスト
- `HB … ccmd=%d`（`wake61` から）
- `BUILD VARIANT cmd_collide=…` / `stall_emu=…` で判別

- ビルド: `firmware/artifacts/remapper_anchor_wake62_collide.uf2`（11/13 22:55,
  `CMD_COLLIDE_US=20000`）

### 歴史の整理（ルディの記憶より）

- 「ポインタの飛び（`BIGDX`）」は 10 月前半から追っていた現象
- **「膠着（`CANSEND-TO` / 0x57 / `ok` 凍結）」の記録は 10/30 以降**
- Wi-Fi ログ (BL-39) の導入は **10/21**

→ **膠着は Wi-Fi 導入後に顕在化した可能性がある**。以前の「Wi-Fi は無関係」という
判断は、症状（飛び vs 膠着）を取り違えていた疑いがある。

## 2026-11-13 `wake61`: クレジット枯渇の「引き金」を探る計装 + Flash 書き込みストレス

ルディの指摘「膠着は Wi-Fi ログを始める前からあった」により、**Wi-Fi 共存説は
主因ではない**ことが確定（gSPI 共有は要因の 1 つにすぎない）。そこで引き金の
候補を直接叩く計装を入れた。

### 22:24 の膠着で分かったこと

```
22:24:28  CANSEND-TO … ccmd=1      ← クレジットはまだ 1 個あった
22:24:28  PROACT → gap_disconnect  ← ここで切断コマンドを送信
22:24:31  ADV-NOACK retry=1 ccmd=0 ← その応答 (Command Status) が来ず 0 のまま
```

**落ちたのは「切断コマンドの応答」**。コマンド自体は送れている。
また膠着直前の `lmax=4ms`（ループ停止なし）なので、**メインループの停止が原因でもない**。

### 追加した計装

1. **`BLE-HB` に `ccmd=%d`** … クレジット残量の時系列（15 秒ごと）
2. **`BT FLASH-WRITE kind=… ms=…`** … TLV (Flash) 書き込みの所要時間。
   Flash 書き込み中は XIP が止まり CYW43 ドライバが動けない仮説の検証
3. **`BT FLASH-STRESS n=… ms=…`** … `-DFLASH_STRESS_MS=10000` で 10 秒ごとに
   ダミーの TLV 書き込みを強制する**因果テスト**。膠着の頻度が増えれば
   「Flash 停止 → HCI イベント取りこぼし」が確定する
4. `BUILD VARIANT flash_stress=…` で焼き間違い検出

- ビルド: `firmware/artifacts/remapper_anchor_wake61_flashstress.uf2`
  （11/13 22:40、`FLASH_STRESS_MS=10000`）
- 注意: Flash 書き込みは 10 秒ごと = 約 8,600 回/日。**数日の実験まで**にする

## 2026-11-13 `wake60`: クレジット枯渇に対して HCI 電源の入れ直しで復旧を試す

`wake59` の観測で**原因が確定**した（22:24 の膠着）:

```
22:24:28  CANSEND-TO … ccmd=1        ← 詰まり始めはクレジットあり
22:24:31  BT ADV-NOACK retry=1 ccmd=0   ← 3 秒後には 0（枯渇）
22:24:40  BT DEAD-SNAP … ccmd=0
22:24:40  BT AUTO-REBOOT              ← 8 時間稼働 → 再起動（復帰）
```

**原因**: BTstack の HCI コマンドクレジット（`num_cmd_packets`）が枯渇。
BLE コントローラのクレジットは 1 個なので、`Command Complete` を 1 回取りこぼすと
**以降の HCI コマンドが一切送れなくなる**（gSPI 共有バスでの取りこぼし）。

**BTstack のソース確認**: `hci_power_enter_initializing_state()` が
`num_cmd_packets = 1` に戻す。つまり **`hci_power_control(ON)` でクレジットが復活する**。

**対策（復旧の段階化）**:

1. `PROACT` で切断（従来）
2. **HCI 電源の入れ直し**（`ctrl_reset_pending` + `ctrl_reload_btcore=false`）
   - ファーム再投入はしない（クレジットだけ戻せばよい）
   - `BT HCI-RECYCLE ccmd=0` を記録
   - 所要 1〜2 秒（再起動の 25 秒より速い）
3. 15 秒戻らなければ従来どおり再起動（`wake58` の経路）
4. 稼働 60 分未満なら再起動しない（USB 運用 + `DEAD-WAIT`）

- ビルド: `firmware/artifacts/remapper_anchor_wake60_hcirecycle.uf2`（11/13 22:30）

## 2026-11-13 `wake59`: HCI コマンドクレジット（ccmd）を観測

ルディが調べた「CYW43 で HCI が受け付けられない原因」の候補のうち、
**gSPI 共有バスでの `Command Complete` 取りこぼし**が症状と一致する:

- BTstack はコマンド送信でクレジットを 1 消費し、`Command Complete` で戻す
- BLE コントローラのクレジットは通常 **1 個**なので、**1 回取りこぼすと以降の
  HCI コマンドが一切送れなくなる**
- 説明できる症状: 広告・切断コマンドが通らない / チップは生きているのに沈黙 /
  再起動でしか直らない / 確率的に起きる

**確認方法**: `hci_can_send_command_packet_now()` を記録する。
これが `false`（`ccmd=0`）ならクレジット枯渇が原因で確定。

- `BT CANSEND-TO … ccmd=%d`
- `BT ADV-NOACK retry=%u ccmd=%d`
- `BT DEAD-SNAP … adv_ok=%d ccmd=%d`

**もし確定したら**: `pico_cyw43_arch_lwip_threadsafe_background` →
`lwip_poll`（メインループで CYW43 を回す = EhHHKB2 と同じ方式）に変えて
イベント取りこぼしの発生自体を止められる可能性がある。

- ビルド: `firmware/artifacts/remapper_anchor_wake59_ccmd.uf2`（11/13 14:20）

## 2026-11-13 `wake58`: PROACT 後の無反応を直接検出（自動再起動の 2 度目の取りこぼし）

**症状**: 11:53:02 に slot2 へ切替 → 一度は接続（11:53:04 `CONNECTED slot=2`）→
11:53:08 に `CANSEND-TO` → `PROACT` で切断要求 → **以降 DISC も ADV-EN も来ない**。
`fl=0x3F`（`0x01` = CONNECTED のまま + `0x20` = tx_stalled）。`DEAD-SNAP` /
`AUTO-REBOOT` は 0 件。

**原因**: `wake57` は `adv_pending_since_us`（広告要求のタイマ）で判定していたが、
この経路では `adv_set(1)` が呼ばれない（切断完了が来ないので切断ハンドラも走らない）。
さらにアプリの状態は **CONNECTED のまま**なので「未接続のときだけ」の条件でも外れる。
結果、どの監視も武装せず発動しない。

**修正**: 監視を「時間」ではなく **PROACT という事象**に紐づける。

- `PROACT` で `proact_pending_us = now` を記録（専用変数。他の経路で消えない）
- 次のいずれかで解除: 切断完了イベント / 接続成立 / 送信成功（`health_ok` が進む）
- **15 秒たっても解除されない** = コントローラが HCI を受け付けていない → DEAD 判定

`BLE_PROACT_DEAD_MS`（既定 15000）で調整可。

**11/12 23:04 のケース**（PROACT 後 57 秒で `DISC rs=0x08` が来て自然復旧）は、
今回の修正だと 15 秒で DEAD 判定 → 再起動になる。復旧は速くなるが「自然復旧を待つ」
挙動ではなくなる（許容と判断）。

- ビルド: `firmware/artifacts/remapper_anchor_wake58_autoreboot.uf2`（11/13 12:05）

## 2026-11-13 `wake57`: 切断完了が来ない膠着も検出する（自動再起動の取りこぼし修正）

**症状**: 11/13 02:10 に膠着したが `AUTO-REBOOT` が出ず、手で抜き差しした。

```
02:10:40  CANSEND-TO -> PROACT（切断要求）
02:10:40  XPORT ble=0
          ← DISC（切断完了イベント）が来ない
          ← 広告の再開も走らず、ADV-EN 待ちの監視も始まらない
```

**原因**: `wake55/56` の監視は **`adv_set(1)` を呼んだ時点**で武装し、**未接続のとき
だけ**判定していた。切断完了イベント自体が来ないケース（＝チップが固まった）では
`adv_set(1)` が呼ばれず、永久に発動しない。

**修正**:

- 監視の意味を「**広告が立ち上がるはず**」に変更し、次の 3 点で武装:
  - `adv_set(1)` を呼んだとき
  - **`PROACT` で切断要求を出したとき**（新規）
  - **切断完了ハンドラ**（新規）
- 「未接続のときだけ」の条件を撤去（切断完了が来ないケースを捕まえるため）
- 正常な競合（接続成立・ADV-EN 成功）では従来どおり即クリア

**11/12 23:04 のケース**は 57 秒後に `DISC rs=0x08` が来て自然復旧していた（＝チップは
死んでいなかった）。今回の修正は「DISC も来ない」ケースだけを追加で捕まえる。

- ビルド: `firmware/artifacts/remapper_anchor_wake57_autoreboot.uf2`（11/13 02:20）

## 2026-11-12 `wake56`: DEAD 監視の誤検出を防ぐ（未接続時のみ監視）

`wake55` の落とし穴: 広告を有効化した直後にホストが接続すると、コントローラが
`LE Set Advertise Enable` に **0x0C (Command Disallowed)** を返すことがある。
この場合 `ADV-EN status=0x00` は永遠に来ないので、**接続できているのに DEAD 判定**
→ 60 分後に**身に覚えのない再起動**が起きる。

**修正**:

- 監視は `con_handle == HCI_CON_HANDLE_INVALID`（未接続）のときだけ
- 接続成立時に `adv_pending_since_us = 0` / `adv_fail_count = 0` / `ble_dead_declared = false`
  をクリア（広告は不要になるため）

これで「切断後に広告が出ない」ケース（10/31・11/9・11/12 の実際の症状）だけを捕まえる。

- ビルド: `firmware/artifacts/remapper_anchor_wake56_autoreboot.uf2`（11/12 21:00）

## 2026-11-12 `wake55`: 広告が出ない膠着の検出と自動再起動

**症状**（20:34）: `CANSEND-TO` → `PROACT` で切断したあと、**二度と広告が出ない**。
スロット切替も効かず（`ADV-ADDR`/`ADV-EN` が 0 件）、`RECONN-TO` で休眠。手で抜き差し
するまで復旧しない。

**ここまでの結論**: ①②③（接続回数・リーク・熱）はすべて否定済み。電波（RSSI -46〜-62）
も主因ではない。残るのは CYW43 の BT 側の内部状態で、**HCI 再投入では戻らない**
（10/31・11/9・11/12 の 3 回で再現）。

**方針変更**: `cyw43_deinit/init` ではなく **自動再起動**を採用。手で抜き差しすれば
100% 直る＝リセットで直ることが実証済みで、実装も検証も単純。

### 実装

1. **検出**: `adv_set(1)` から 3 秒待って `ADV-EN status=0x00` が来ない → 1 秒間隔で
   最大 3 回張り直す → それでも来ない = **コントローラが HCI を受け付けていない**
   （`BT ADV-NOACK retry=n`）
2. **記録**: `BT DEAD-SNAP up=.. conn=.. heap=.. temp=.. rssi=.. slot=.. bonded=.. ok=.. cp=.. disc=.. rs=0x.. acl=.. blk=.. adv_ok=0`
3. **再起動**: 稼働 **60 分以上**なら 500ms 待って（ログ送信の猶予）`watchdog_reboot()`。
   `BT AUTO-REBOOT adv_stuck up=..`
4. **60 分未満なら再起動しない**。死んだ状態を保持したまま USB で運用し、
   5 分ごとに `BT DEAD-WAIT up=..` を出す（＝デバッグのチャンスを潰さない）

**設計のポイント**: ループ防止を「起動後 60 分は再起動しない」だけにしたので、
**再起動を跨いだ状態保存が不要**（`__uninitialized_ram` 不使用）。ルディの指摘
「跨いで保存したくない」を満たす。

- ビルド: `firmware/artifacts/remapper_anchor_wake55_autoreboot.uf2`（11/12 20:47）
  - 基準: `wake54` と同じ（アンカー + BL-43 設定 OFF + `lat_watch=off`）
  - 判別: `BUILD VARIANT bl43_flowctl=off` / `lat_watch=off`

## 2026-11-12 `wake54`: LAT-HIGH（自発再接続）を無効化

**理由**: `wake43` で入れた `LAT-HIGH`（lat 30ms 以上が 60 秒続いたら自発切断）が
**1 日 8 回**発動していた。しかも 14:08 のケースでは

```
14:08:49  LAT-HIGH 36ms x6 -> proactive reconnect   ← 自分から切った
14:08:50  CONNECTED（0.3 秒で再接続）
14:08:52  CANSEND-TO hci_idle=2008ms -> PROACT     ← 2.5 秒後にはもう詰まった
```

**逃げた先も死んでいた**。3 件の本物の詰まり（14:08 / 15:18 / 16:12）を
LAT-HIGH は 1 件も防げておらず、切断チャーンだけを増やしていた。
ルディの体感「ちょいちょい不安定」の一因。

**対応**: `-DBLE_LAT_WATCH_DISABLE=ON` で `BLE_LAT_WATCH_ENABLE=0` にし、
LAT-HIGH のブロックごとコンパイルアウト。`PROACT`（2 秒不通で切断）は残す。

### 同日の観測（①②③ の否定）

```
14:08  CANSEND-TO hci_idle=2008ms cp_idle=2090ms rlog_age=184ms rssi=-46 conn=8  heap=63732 temp=35
15:18  CANSEND-TO hci_idle=2009ms cp_idle=2080ms rlog_age=76ms  rssi=-55 conn=6  heap=63612 temp=36
16:12  CANSEND-TO hci_idle=2009ms cp_idle=2083ms rlog_age=830ms rssi=-47 conn=3  heap=63572 temp=35
```

- `conn` が 3 / 6 / 8 とバラバラ → **① 接続サイクル説は否定**
- `heap` は起動時 63540 から 6 時間で +192 バイト → **② リーク説も否定**
- `temp` 35℃ で安定 → **③ 熱説は否定**（密閉筐体でも問題なし）
- `rssi` は -46 でも発生 → **電波も主因ではない**（iPad を置いても R荒れは変わらず）

残るのは **CYW43 内部の BT 側の状態**のみ。次の一手は `cyw43_deinit()` +
`cyw43_arch_init()` によるチップ丸ごと再初期化。

- ビルド: `firmware/artifacts/remapper_anchor_wake54_nolat.uf2`（11/12 16:40）
  - 基準は `wake52_noflow` と同じ（アンカー + BL-43 設定 OFF）
  - `BUILD VARIANT lat_watch=off` で判別

## 2026-11-12 `wake53`: ソークの基準をアンカーから HEAD に移す（BL-45 の修正を取り込む）

**症状**: `wake52_noflow` で slot2 に切り替えたら**広告が出ず接続できない**。

```
12:19:32  SLOT-APPLY slot=2    addr=E4:08:43:8B:22:2F
12:19:32  APPLY-DONE st=2
          ← ADV-ADDR / ADV-PARAM / ADV-EN が 1 つも出ない
```

slot1 のアドレスは `…27:31`、slot2 は `…22:2F`。**切り替えのたびに MAC を変えている**。
これは BL-45 で特定した「実行中にアドレスを変えると広告が出ない」現象そのもの。

**原因**: ソークビルドは**アンカー `3a8d85b`（10 月末）+ パッチ**から作っている。
この地点には BL-45 の修正が入っていない（`get_slot_random_address()` で
スロットごとに静的ランダムアドレスを生成し、実行中に切り替える旧方式）。

一方、**ツリーの HEAD は BL-45 修正済み**（`gap_local_bd_addr()` の公開アドレスを
固定し、accept list でスロットを分離）。つまり**修正はツリーにだけ入っていて、
実機で動かしているソークビルドには反映されていなかった**。

**対応**: ソークの基準をアンカーから **HEAD に移行**。`wake37`〜`wake52` の修正は
すべてツリーにコミット済みなので、パッチを当て直す必要がない。

- `firmware/artifacts/remapper_head_wake53.uf2`
  （HEAD + `ENABLE_REMOTE_LOG` + `BLE_LOCK_DISABLE`、BL-43 フロー制御は既定=ON）
- `firmware/artifacts/remapper_head_wake53_noflow.uf2`
  （同上 + `BL43_NO_FLOW_CONTROL=ON`。フロー制御 A/B を継続する場合）

確認済みマーカー: `BT ADDR public`（BL-45 修正）/ `ADV-FILTER`（accept list）/
`bl43_flowctl=off`（noflow 版のみ）。

## 2026-11-12 `wake52`: BL-43 フロー制御設定の A/B（EhHHKB2 との差分から）

ルディの「EhHHKB2 にヒントは無いか」から `~/git/EhHHKB2` を読んだところ、
**`btstack_config.h` がうちと決定的に違っていた**:

| 設定 | EhHHKB2 | うち (BL-43) | BTstack 既定 |
|---|---|---|---|
| `MAX_NR_CONTROLLER_ACL_BUFFERS` | 未定義 | **3** | **255** |
| `ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL` | 無効 | 有効 | 無効 |
| `HCI_HOST_ACL_PACKET_LEN/NUM` | 未定義 | 1024 / 3 | — |

EhHHKB2 は**この対策を一切せずに動いている**。当時の BL-43 は A/B をしておらず、
しかも 10/22 の BL-45 切り分けで「**flow control を無効化しても症状は変わらない**」と
記録が残っている（ただしそれは「広告が出ない」問題の話で、膠着の A/B は未実施）。

**つまり BL-43 の設定は「効いた」証拠が無いまま残っている。**

### つくったもの

- `firmware/CMakeLists.txt` に `BL43_NO_FLOW_CONTROL` オプション
  （BTstack 側のソースも `btstack_config.h` を読むため、`pico_btstack_ble` /
  `pico_btstack_cyw43` にも INTERFACE 定義を渡す）
- `btstack_config.h` の BL-43 ブロックを `#if !defined(BL43_NO_FLOW_CONTROL)` で囲む
- 判別用に `BUILD VARIANT bl43_flowctl=off` をログに出す

### A/B 用の 2 本

| ファイル | 内容 |
|---|---|
| `remapper_anchor_wake52_flowctl_on.uf2` | 従来（BL-43 設定あり、`ACL_BUFFERS=3`） |
| `remapper_anchor_wake52_noflow.uf2` | BL-43 設定なし（EhHHKB2 と同じ、既定 255） |

どちらも他の修正（`wake47`〜`wake51` の観測・`PROACT`・`connect=1...`）は同一。

**判定**: 数日ずつ動かして `CANSEND-TO` の回数を比べる。`noflow` 側で減れば
BL-43 の設定が膠着の一因だったと確定する。

## 2026-11-12 `wake51`: 寿命仮説の切り分け（conn / heap / temp を記録）

ルディの指摘: 「接続回数で死ぬなら寿命を延ばすだけ。**そもそも寿命がある設計**が
あるのではないか」。もっともなので、対策ではなく**切り分け用の観測**を追加した。

| 仮説 | 見分ける指標 | 今回追加 |
|---|---|---|
| ① 接続サイクル | 起動後の接続成立回数 | `conn=` |
| ② 資源リーク | ヒープ使用量の推移 | `heap=` (mallinfo().uordblks) |
| ③ 熱 | チップ温度 | `temp=`（RP2040 内蔵センサ, ADC4） |
| ④ 稼働時間 | `up=` と失敗の相関 | 既存 |

- `BLE-HB` の末尾と `CANSEND-TO` の行の両方に出す
- ADC は温度センサ用に初回だけ初期化（`hardware_adc` をリンク追加）
- 挙動は一切変えない（観測のみ）

**判定**: 次の失敗で `conn=` が毎回同じ値なら①、`heap=` が単調増加なら②、
`temp=` が高ければ③。どれも動いていなければ④（時間依存）を疑う。

- ビルド: `firmware/artifacts/remapper_anchor_wake51.uf2`（11/12 09:48）

### 11/12 09:30 台の観測

- 09:32:53 slot2 接続 → 6 分稼働 → 09:38:50 slot1 へ切替 → `rs=0x22` →
  09:39:30 slot2 → 0.09 秒で `rs=0x08`。**再起動後 4 サイクルで死んだ**ように見える
- 09:28:46 にも `CANSEND-TO hci_idle=2009ms` → PROACT → USB フォールバックは機能
  （`fwd` 増加）。`u=1/1` で USB も生きていた

## 2026-11-11 `wake50`: USB 未 configure の再アタッチを諦めない

**症状**: 21:45 の BLE 断で USB フォールバックが働かなかった。ログを見ると
**`wake49` を焼いた 11/09 23:07 から 11/11 21:46 まで、2 日間ずっと `u=0/0`**
（`USB DEV configured` が 0 件）。行き先が無いので、どれだけ早く切り替えても無意味だった。
なお `CANSEND-TO` → `BT XPORT ble=0` は 19 ms で切り替わっており、`wake49` 自体は機能。

**原因**: `main.cc` に USB 再アタッチ機構はあった（`08f720c`）が、
**5 回で打ち切る**設計で、しかも**起動直後の 3 秒間で 5 回を使い切っていた**
（ホストがまだ列挙中）。フラッシュ直後に macOS が configure に失敗すると、
その後は二度と試さず永久に死んだまま。

**修正**:

- 最初の再アタッチは起動 5 秒後まで待つ
- 速い再試行 5 回（3 秒間隔）→ それでも駄目なら **60 秒間隔で無期限に**続ける
- `USB DEV re-attach n=` をログに追加

これで焼き直し直後・スリープ復帰後でも、最大 60 秒で USB が生き返る。

- ビルド: `firmware/artifacts/remapper_anchor_wake50.uf2`（11/11 21:55）

### 11/11 21:45 の観測

```
21:45:56  CANSEND-TO n=1 hci_idle=2009ms cp_idle=2026ms rlog_age=346ms rssi=-62
21:45:56  CANSEND-PROACT -> disconnect
21:45:56  BT XPORT ble=0            ← 19ms で USB へ（修正は機能）
         しかし u=0/0 なので出力先なし → 無反応
```

`hci_idle=2009ms` は今回も「HCI イベントが 2 秒来ない」。ただしアイドル時は
そもそもイベントが来ないため、この値だけでは「チップ停止」と断定できない
（10/31 の `rlog_age=387ms` の件と合わせて、次はアイドル状態との比較が必要）。

## 2026-11-09 `wake49`: CAN_SEND_NOW 不通で即 USB フォールバック＋表示を正直に

**症状**（ルディ報告）: 「USB フォールバックも効いてない」「接続済と勘違いしている」。

**原因**: 転送先の判定は

```c
curr_ble_connected = ble_hid_is_connected() && !ble_hid_is_tx_stalled();
```

だが `ble_hid_is_connected()` は**アプリの状態フラグ**（切断イベントが来たか）しか
見ない。今回のように**チップが黙ってリンクが死ぬ**（切断イベントが来ない）場合、
`CONNECTED` のまま固定され、送信は BLE に流れて `ble_can_send` 待ちで捨てられ、
**USB には一切行かない**。OLED も `BLE-FAST` のままで「繋がっている」表示になる。
`health_tx_stalled` は 120 秒の膠着判定でしか立たなかった。

**修正**: `CANSEND-TO`（送信要求が 2 秒通らない）の時点で `health_tx_stalled = true`
にする。これで

- 即 USB にフォールバック（USB が生きていれば 2 秒で復帰）
- OLED は `USB-OUT` / `NO-HOST` に切り替わる
- 送信が 1 回でも成功すれば `health_ok` が進み自動で BLE に戻る

（USB 自体が `u=0/0` のときは何も出せないので `NO-HOST` 表示。挿し直しは必要）

- ビルド: `firmware/artifacts/remapper_anchor_wake49.uf2`（11/9 23:15）

### 2026-11-09 の観測メモ

- 21:10 / 22:16 / 22:33 / 23:02 と 2 時間で 4 回。いずれも
  `CANSEND-TO … hci_idle=2009〜2010ms cp_idle=2025ms`（HCI イベントが 2 秒沈黙）
- うち 2 回は `CANSEND-PROACT` の切断後に**広告が戻らず電源再投入**が必要だった
- 21:00 以降 Wi-Fi の RSSI が -37 → -47〜-61 に悪化（2.4GHz 環境の変化？）
- USB は `u=0/0`（macOS が configure していない）でフォールバックも死んでいた

## 2026-11-07 `wake48`: 接続直後 2 秒で接続パラメータを再要求

**症状** (21:55〜): 接続 → **0.5〜2 秒で `rs=0x08`** → 再接続 → また切れる、のループ。

```
21:57:02  DISC rs=0x08 → 再接続
21:57:06  CANSEND-TO（PROACT は 30 秒制限で発動せず）
21:57:08  DISC rs=0x08（2 秒）
21:57:10  DISC rs=0x08（1.5 秒）
21:57:11  DISC rs=0x08（0.5 秒）
```

**原因の 1 つ**: 再接続のたびに `PARAM-REQ` の間隔タイマ (30 秒) がリセットされ、
新しい接続は**毎回ホスト既定の `st=1 秒` のまま 30 秒間**もつ。1 秒の監視窓では
少しの乱れで切れるので、切れる → 再接続 → また 1 秒 → 切れる、の弱い状態のループに
入っていた。

**修正**: 接続直後は **2 秒後**に 1 回目の再要求を出す（2 回目以降は従来どおり
30 秒間隔、RESUME 後 60 秒の抑制も維持）。これで `st=400`（4 秒）がすぐ戻る。

- ビルド: `firmware/artifacts/remapper_anchor_wake48.uf2`（11/7 22:10 / `wake47` の
  切り分け情報を含む）

## 2026-11-07 `wake47`: 膠着の切り分け情報を追加 / `wake46` の初成功

### `wake46` の成果（2026-11-07 21:48）

```
21:48:26  LATGAP ms=180
21:48:28.431  CANSEND-TO n=1
21:48:28.451  BT CANSEND-PROACT 2s no grant -> disconnect   ← 先に逃げた
21:48:30.244  DISC rs=0x08                                  ← ホストは 1.8 秒後にタイムアウト
21:48:32.642  CONNECTED                                     ← 4.2 秒で復帰
```

**電源再投入なしで自動復帰した初のケース**。以前は「膠着 → 数分無反応 → 抜き差し」
だったのが 4 秒で戻る。当日の `CANSEND-TO` はこの 1 回のみ。

### 追加した観測（挙動は変えない）

`CANSEND-TO` の行に、その瞬間の状態を付けた:

```
CANSEND-TO n=1 hci_idle=12ms cp_idle=2041ms rlog_age=3800ms rssi=-42
```

| 項目 | 判定 |
|---|---|
| `hci_idle` が大きい | **チップ/ドライバが停止**（CYW43 内部・バス） |
| `hci_idle` は小さいが `cp_idle` が大きい | **電波/相手が応答しない**（ホストが ACK を返さない） |
| `rlog_age` が小さい | **Wi-Fi 共存**（UDP 送信と重なった） |

`remote_log_tx_age_ms()` / `remote_log_last_rssi()` を新設（`ENABLE_REMOTE_LOG=OFF`
時はインラインでダミー値を返す）。

### `HAVE_ASSERT` は見送り

参考プロジェクト（`picow_ble_hid_keyboard`）は `HAVE_ASSERT` を定義しているが、
Pico では `assert()` → panic で停止し、**UART 未接続の現運用では何も残らない**。
導入するなら UART (GP16/17) を繋いだ検証時。

- ビルド: `firmware/artifacts/remapper_anchor_wake47.uf2`（11/7 22:00）

## 2026-11-06 `wake46`: CAN_SEND_NOW 2 秒不通で自発切断（膠着の入口で逃げる）

**観測**: 11/6 01:38 と 09:19 で同じ形の膠着。どちらも **最初の症状が `CANSEND-TO`
（CAN_SEND_NOW が 2 秒来ない）**で、その 20 秒後に `NORESTORE`（`wake45` の新判定）が
働き、即 USB にフォールバックした。**検知は 19 秒（以前は 11 分）**。

| 時刻 | 出来事 |
|---|---|
| 01:38:33 | `RESUME gap=15s`（無操作から再開） |
| 01:38:43 | `CANSEND-TO n=1` → `BIGDX dx=434 dy=754` |
| 01:39:02 | `CREDIT-STALL` → `NORESTORE cp frozen 20s` → 再投入 → `st=1` で停止 |
| 09:19:01 | `CANSEND-TO n=1` → `BIGDX dx=438 dy=38` |
| 09:19:19 | `NORESTORE cp frozen 20s` → 再投入 → `st=1` で停止 |

**判明**: BT コア再投入は `STACK st=1` で止まり、**コントローラは戻らない**（4 回連続）。
復旧は電源再投入のみ。

**対策 (`wake46`)**: `CANSEND-TO` の**1 回目（2 秒不通）で自発的に切断**して
再広告する。チップが固まる前（数十秒の余裕がある）に逃げる狙い。誤発動の連鎖を
防ぐため 30 秒に 1 回まで。`BLE_CANSEND_PROACT_ENABLE=0` で無効化。

**リスク**: ホストが正当に 2 秒沈黙する場面（スリープ等）で切断する可能性。
その場合は 0.5 秒で再接続するので実害は小さい見込みだが、頻発するようなら
しきい値か無効化で調整する。

- ビルド: `firmware/artifacts/remapper_anchor_wake46.uf2`（11/6 09:22）

## 2026-11-05 `wake45`: 膠着判定を cp 停滞ベースに変更（絞りが判定を飢えさせた）

**症状**: 12:31 に BLE 不調。実際には **12:22:35 から `cp` が凍結**していたが、
復旧（`FLOWCTL-WEDGE`）が **12:33:28 まで発火しなかった**（11 分間）。

**経緯** (`bl39_1104_2259.log`):

```
12:21:11  CREDIT-STALL start (storm aclrun=50 acl=50)   ← 1 回目の膠着
12:21:12  DISC rs=0x08 → 0.4 秒で再接続
12:21:13  CREDIT-STALL start (storm aclrun=51 acl=0)    ← ★再接続 20ms 後に再発動
          （aclrun が前の接続の値を引きずっていた = 誤検知）
12:22:35  cp=23588 で凍結（ここから送信不成立）
12:33:28  FLOWCTL-WEDGE acl=73 → 再投入 → STACK st=1 で停止（復旧せず）
```

**原因**: `CREDIT-STALL` で送信を 1 秒 1 回に絞ると、**0x57 の回数 (`acl`) が
ほとんど増えなくなる**。膠着判定は `acl` の増加 (40 回) を条件にしていたため、
条件が満たされず 11 分間検知できなかった。**対策が判定を飢えさせた**形。

**修正 (`wake45`)**:

- `Completed Packets` が最後に動いた時刻 (`cp_last_change_us`) を追跡し、
  **`credit_stall` 中に `cp` が 20 秒動かなければ膠着**と判定して再投入
  （`acl` の増加に依存しない）
- 接続時に `health_acl_run` を 0 に戻す（再接続直後に stale な値で誤って絞らない）

- ビルド: `firmware/artifacts/remapper_anchor_wake45.uf2`（11/5 12:40）
- 残: 再投入後 `STACK st=1` で止まる問題（コントローラが戻らない）。
  次の候補は `cyw43_deinit()` + `cyw43_arch_init()`

## 2026-11-04 `wake44`: ボンド消去直後に旧ホストが再接続する不具合

**症状**（ルディ報告）: slot 4 のボンドを消去 → OLED は `Slot Cleared` → しかし
**すぐ slot 4 で同じホストに繋がり、暗号化も通る**。

**ログ** (`bl39_1103_1754.log`):

```
22:46:06.626  BONDCLR slot=4          ← 消去
22:46:06.669  DISC rs=0x16            ← こちらから切断
22:46:06.895  ADV-EN ... slot=4 pair=0  ← ★広告が出てしまう
22:46:06.924  LE-CONN slot=4 peer=F0:2F:4B:09:C4:08
22:46:07.274  ENCR ok slot=4          ← 暗号化成功（コントローラに鍵が残っている）
```

**原因**: 切断完了ハンドラが「切断直後に広告を張り直す」ため `adv_enable_pending` を
200ms 遅延で予約する。その後「未ボンドなので STANDBY_SILENT」と判定して `adv_set(0)`
するが、**予約を取り消していない**ので 200ms 後に `adv_set(1)` が発火し、ボンドを
消した直後の一瞬だけ広告が出る。そこへ旧ホストが接続していた。

**修正**: 未ボンドで静音に落とす分岐で `adv_enable_pending = false` を必ず行う。
これで消去後は広告が出ず、旧ホストは接続できない（2 段階ホットキーの 2 回目で
ペアリングモードに入れたときに初めて広告する）。

- 補足: コントローラ側には旧 LTK が残るため、アドレスを変えない限り「接続できれば
  暗号化も通る」。CYW43439 は実行中の広告アドレス変更が効かない (BL-45) ので、
  広告を止めるのが唯一の確実な遮断になる
- ビルド: `firmware/artifacts/remapper_anchor_wake44.uf2`（11/4 22:55）

## 2026-11-03 `wake43`: lat 高止まりを前兆として自発再接続

**15:26 の膠着** (`bl39_1101_1455.log`, 稼働 48 時間時点):

- 15:26:26 に `cp` が凍結 → 15:26:33 `BT CREDIT-STALL start (storm aclrun=50 acl=50)`
  → **`acl` は 50→66 で停止**（修正前の 00:42 は 1261 回）。洪水を止める対策は成功
- 15:28:29 の 120 秒 `FLOWCTL-WEDGE` → BT コア再投入は `STACK st=1` → `st=2` まで
  進むが **`ADV-ADDR`/`ADV-PARAM`/`ADV-EN` が 1 つも出ず**、30 秒後に休眠。
  `wake36`〜`wake41` と同じ「HCI 再投入では広告機能が戻らない」= 復旧は未達
- USB フォールバックは正常（`fwd` 増加、OLED は `USB-OUT`）
- **前兆を発見**: 膠着の 6 分前（15:20:39）から `lat`（CAN_SEND_NOW 待ち）が
  36〜50ms に上昇し続けていた。正常時は 0ms

**対策 (`wake43`)**: `lat` の 10 秒窓最大値が **30ms 以上で 60 秒継続**したら、
チップが固まる前に `gap_disconnect` で**自発的に切断 → 再広告**する。
`BLE_LAT_WATCH_ENABLE=0` で無効化、`BLE_LAT_HIGH_MS` / `BLE_LAT_HIGH_WINDOWS` で
しきい値を変更できる。ログは `BT LAT-HIGH <ms> x6 -> proactive reconnect`

- ビルド: `firmware/artifacts/remapper_anchor_wake43.uf2`（11/3 15:35）
- 未着手: `cyw43_deinit()` + `cyw43_arch_init()`（チップ丸ごと再初期化）。
  Wi-Fi/ログ/BTstack を巻き込むため、`wake43` で前兆検知が効かなかった場合の次段

## 2026-11-01 `wake42`: `aclrun` 欠落の修正 / 復帰直後の PARAM-REQ 抑制 / ビルド自己検算

### 発覚した不具合（自分のミス）

`wake41` の実機 soak で 13:23 に膠着したが、**`aclrun=0` のまま `acl` だけが 511 まで
増えた**。原因は `wake37` で `health_acl_run++` を `build_anchor.py` にミラーし忘れて
いたこと（ツリー側には入っていた）。その結果:

- `aclrun=` は **wake37〜wake41 まで常に 0**（HB の観測値が嘘だった）
- `wake41` の「0x57 が 50 連続したら送信を絞る」は **一度も発動しない**
- 13:23 の膠着は 120 秒待つ既存経路まで進み、0x57 を 511 回撒いた

**対策**: `build_anchor.py` の末尾に **生成物の自己検算**を追加。必須マーカー
（`health_acl_run < 0xFFFF` / `BT CREDIT-STALL start (storm` / `BT NORESTORE` /
`BT ADV-VERIFY armed` / `BT CTRL-DEAD` / `last_resume_us` / `USB DEV %s` /
`clat=%u st=%u`）が生成物に無ければ **ビルドを中断**する。

### 13:20 の Mac 復帰で観測したこと

- **12 時間のスリープを無切断で跨いだ**（`ok=6930` のまま、`tx=43,343,693ms`）
- 復帰直後に約 3 秒の詰まり → `CANSEND-TO` → `DISC rs=0x08` → 0.8 秒で再接続（正常復帰）
- ただし復帰後 40 秒で **0x08 が 3 回**。うち 2 回は `PARAM-REQ`/パラメータ適用の
  直後だった（因果は断定できないが、復帰直後は不安定）
- → **RESUME 後 60 秒は `PARAM-REQ` を撃たない**（`last_resume_us`）

### 13:23 の膠着で分かった復旧経路の限界

`FLOWCTL-WEDGE` → BT コア再投入で **`STACK st=1` から 32 秒以上進まない**（前回 00:44 は
`st=2` まで到達して広告で失敗）。HCI 層の再投入では戻らない領域があることが確定。
次段は `cyw43_deinit()` + `cyw43_arch_init()`（WL チップ丸ごと再初期化）。

- ビルド: `firmware/artifacts/remapper_anchor_wake42.uf2`（11/1 13:26）

## 2026-11-01 `wake41`: 0x57 ストーム検知 / 早期再投入 / 再投入後の広告検証 / USB 可視化

**引き金**: 2026-10-31 00:42 に `DISC rs=0x08` → 即再接続 → その接続は
**クレジットが返らない壊れたリンク**（`cp=116223` で凍結、0x57 が 1261 回）→
120 秒後の `FLOWCTL-WEDGE` で BT コア再投入したが **`STACK st=2` まで戻ったのに
`ADV-EN` の完了が返らず広告が出ないまま休眠** → 抜き差しで復旧。

- **0x57 ストーム検知**: `credit_stall` を「CAN_SEND_NOW タイムアウト」だけでなく
  `health_acl_run >= 50`（連続 0x57）でも発動。今回の膠着は CAN_SEND_NOW が
  届き続けるため、旧条件では一度も発動しなかった（判定条件のずれ）
- **早期再投入**: 絞っても（1 秒 1 回のプローブでも）20 秒間 `Completed Packets` が
  1 つも返らないなら「壊れた再接続」と判定して即 BT コア再投入
  （`BT NORESTORE cp frozen 20s`）。120 秒待つ間に 1261 回撒いてコントローラが
  広告コマンドを受け付けなくなったため早期化
- **再投入後の広告検証**: `STACK WORKING` 到達後に 1.5 秒待ち、`ADV-EN` が無ければ
  広告を張り直す（最大 3 回、`BT ADV-RETRY`）。それでも出なければ
  `BT CTRL-DEAD` を出して OLED に `POWER CYCLE` を表示（黙って休眠しない）
- **USB configure 状態の可視化**: `USB DEV configured/unconfigured` をログに追加。
  OLED は BLE 膠着で USB フォールバック中なら `BLE-FAST` ではなく USB/NO-HOST を出す
- **AGENTS に追加**: 焼いた直後に `u=1/1` を確認、`0/0` なら USB を挿し直す
  （フラッシュ時に macOS が configure を失敗したまま固定される事象。同じバイナリが
  挿し直しで `u=1/1` に戻ることを実測）
- ビルド: `firmware/artifacts/remapper_anchor_wake41.uf2`（11/1 00:52）

## 2026-10-31 夜 soak (`wake40`): 9.5 時間・切断 0 件 / Mac スリープ 4.6 時間を跨いで復帰

`bl39_1031_1250.log` (12:50 起動 → 22:17 時点で up=34039s = 9.45 時間)。

- **`DISC` 0 件**。`ACLFULL` / `CREDIT-STALL` / `CANSEND-TO` / `FLOWCTL` / `OLED REINIT` も 0
- **Mac スリープ 4.6 時間を無切断で跨いだ**: 17:50 頃から 22:16 まで `ok`/`cp` が
  完全凍結 (`tx=16622744ms`) → `BT RESUME gap=16642s` で**即復帰**、`DISC` なし
  - 15 分スリープ (13:01→13:16, `RESUME gap=919s`) も同じく切断なし
  - 以前はスリープ中に 0x57 が溜まり `rs=0x08` で切れ、再接続で膠着 → 抜き差し、
    という連鎖だった。**リンクが生き残るようになった**
- `cu=3` が 9.5 時間変化なし = ホストとのパラメータ押し合いなし
- 短い停止 (`BLE STALL`) 6 件。いずれも 1〜6 秒で自然復帰 (0x57 は 0 件)
- OLED スクリーンセーバーも仕様どおり: 60s → `stage=1`、120s → `stage=2`、
  操作で `stage=0`（毎秒の誤発動は消えた）
- 定常状態の円: 入力 193/s → 送信 **66.9/s**、`gmax` 11ms、`omax` 13ms、
  `dmax` 150〜214、`clat=0` / `st=400` / `lat=0ms`
- 復帰直後の最初の HB だけ `dmax=422`（スリープ中に溜まった移動のまとめ送り）
- **結論**: 断続切断の主因は **ホスト既定の supervision timeout 1.0 秒**だった。
  Apple 準拠の `(12,12,0,400)` を周期側から要求して `st=400` に定着させたことで、
  スリープ跨ぎでもリンクが生き残るようになり、**0x57 膠着の引き金
  (スリープ明けの連続 0x57) も同時に消えた**
- 未検証: 膠着復旧経路 (`power-cycle+reload`, `wake36` 以降)。膠着自体が
  起きないため実地検証の機会が無い（望ましい状態だが、経路は未確認のまま残る）

## 2026-10-31 `wake40`: OLED スクリーンセーバー誤発動の修正 + パラメータ再要求の周期化

- **OLED の `stage=2` 誤発動を修正**（ルディが気づいた）。実機ログに
  `OLED SS stage=2 idle=4294967s` と `stage=0 idle=0s` が 1 秒ごとに交互に出ていた
  - 原因: `oled_task()` は **Core 1**、`last_activity_ms` は **Core 0**（入力・USB 送信・
    BLE 送信通知）が更新する。Core 1 が `now_us` を取った後に更新されると差分が
    **負**になり、unsigned で 4,294,967 秒の巨大 idle になる → スクリーンセーバー誤発動
    → 輝度（0x05 ↔ 0x8F）とアニメ描画が毎秒切り替わる。「1〜2 行だけ表示される」
    ちらつきの正体もこれの可能性が高い
  - 修正: 差分を `int32_t` で見て負なら 0 に丸める（`idle_signed_ms`）
- **接続パラメータ再要求が一度も走っていなかった**（`wake39` の実測で判明）。
  `wake39` では要求を「Connection Update Complete ハンドラの中」に置いたが、
  このハンドラはホストが更新を出すまで走らないため `PARAM-REQ` は 0 件だった。
  結果は `BT PARAM ci=12 lat=0 st=400 n=1` → 20ms 後に
  `BT PARAM ci=12 lat=4 st=100 n=2` で**ホストが即座に上書き**して終わっていた
  - 修正: `ble_hid_poll` の周期側へ移設。接続中に `latency > 0` のとき 30 秒間隔で
    最大 3 回（1 回目 `ci=12`、2 回目以降 `ci=9` = 11.25ms）
- `tools/uf2_check.py` を追加。`strings` による UF2 検算は 512 バイトブロックの
  ヘッダで文字列が分断され **0 件の誤判定**を起こす（当日実際に踏んだ）。
  ペイロードを連結してから検算する
- ビルド: `firmware/artifacts/remapper_anchor_wake40.uf2`（12:47）

## 2026-10-31 `wake39`: 接続パラメータを Apple 準拠へ / `clat` 実測で仮説を修正

- `wake38` で `clat`/`st` を実測した結果:
  - **`clat=4` でも 66/s 出る** → レビューの「latency 4 で 13.3/s が上限」は**誤り**。
    スレーブレイテンシは「送らなくてよい」の許容であって、送りたいときは毎イベント
    送れる。レート制限の原因ではなかった
  - **`st=100` = supervision timeout が 1.0 秒**。15ms × (1+4) = 75ms で 1 イベント
    なので、1 秒＝13 回連続取りこぼしで切断。**`rs=0x08` の断続切断の主因はこれ**
  - `cu=2` のみ＝こちらの再要求は受理されていない
- **原因**: 要求値 `(9, 12, 0, 400)` は Apple の一般則（`Min ≥ 15ms`、`Min + 15 ≤ Max`）
  から外れており、macOS に丸ごと無視されて既定の `(12, 4, 100)` を押し付けられていた
- **修正**: 要求を `(12, 12, 0, 400)`（15ms / latency 0 / timeout 4 秒、Min=Max で一般則適合）
  に変更。再要求は latency>0 **または timeout<4 秒**のとき 30 秒間隔に制限。
  受理判定用に `BT PARAM-REQ` / `BT PARAM ci=.. lat=.. st=.. n=..` をログに追加
- ビルド: `firmware/artifacts/remapper_anchor_wake39.uf2`（12:41）

## 2026-10-31 `wake38`: OLED の USB ポート表示 P1/P2 を入れ替え（物理配線の都合）

- ルディの配線で P1/P2 の表示が物理ポートと逆になっているため、`update_usb_ports_status()`
  で検出結果と表示ラベルを交換する。`OLED_USB_LABEL_SWAP`（既定 1）で切り替え可能、
  0 にすると基板シルク通りに戻る
- 検知ロジック（GP0/1 = Host 1、GP2/3 = Host 2）はそのまま。表示とログのみ入れ替え
- ビルド: `firmware/artifacts/remapper_anchor_wake38.uf2`（12:32、`wake37` の全内容を含む）

## 2026-10-31 `wake37`: P0 観測 + 0x57 監視の作り直し

- **観測追加** (`BLE-HB` の末尾): `clat=`（実際の connection latency）/ `st=`（supervision
  timeout）/ `cu=`（Connection Update Complete の回数）/ `aclrun=`（連続 0x57 回数）/
  `cpd=`（Completed Packets の増分）。`cpd=0` が続くのが膠着の直接証拠
- **0x57 監視の再設計**:
  - 2 回連続で `gap_disconnect` する挙動を**廃止**（膠着時は切断コマンド自体が
    届かず、生きているリンクを壊すだけだった）
  - 3 回目（6 秒）で `BT CREDIT-STALL start` を出し、**送信を 1 秒 1 回のプローブに絞る**
    （0x57 の洪水を止めてチップとホストに休ませる）
  - クレジットが戻る（送信成功）と自動解除
  - 120 秒の `power-cycle+reload`（`wake36` の BT コア再投入）は据え置き
- `ble_hid_health_t` に `clat`/`st`/`cu`/`acl_run` を追加
- ビルド: `firmware/artifacts/remapper_anchor_wake37.uf2`（12:00）
- **次**: 1〜2 時間の円軌道テストで `clat`/`cpd`/`ok/s` の相関を見る →
  送信結果の伝播 → 接続パラメータ A/B

## 2026-10-31 静的解析レビュー受領 (docs/BLE-REVIEW-2026-10-31.md)

- 別セッションで静的解析した結果を `docs/BLE-REVIEW-2026-10-31.md` に保管
- コードで裏取りした結果、次の 3 点は正しいと確認:
  1. **0x57 監視が構造的に発火しない** — 0x57 のたびに 2 秒タイマーを張り直すため
     （`ble_hid.cc:2035-2045`）、2 秒未満間隔で 0x57 が続く限り
     `can_send_timeout_handler` が走らない
  2. **送信結果が上位に伝播しない** — `ble_hid_send_report` は void、
     `do_send_report_ble` は常に true、`remapper.cc:1448-1461` は失敗でも pop
  3. **接続パラメータ `(9,12,0,400)` は Apple の一般則から外れる**（latency>0 のとき
     10 秒周期で再要求）
- こちらの追加所見: 膠着の実体は **`cp`（Completed Packets）の凍結** ＝ ホストが
  クレジットを返さないこと。タイマーに頼らず `cp` 停滞で検出できる
- 進め方（合意）: `wake36` の検証（`reload=1` → `STACK st=2` → `ADV-EN`）→
  `wake37` で P0 観測（`clat`/`st`/`cu`/`ori`/0x57 連続数/`cp` 増分）＋
  0x57 監視の作り直し（再アーム廃止・5 秒 cp 停滞で送信を 1/s プローブへ）→
  その次に送信結果の伝播 → 接続パラメータ A/B

## 2026-10-31 11:44 膠着で「BT コア再投入」を実機検証 → 順序が悪く半復旧 (`wake36`)

- 11:44:23 に `rs=0x08`（接続監視タイムアウト）→ 再接続 → 2 秒後に `CANSEND-TO` と
  `BIGDX dx=616 dy=584` → `ok` 18130 で凍結、`acl` 880 まで増加（0x57 膠着）
- 11:46:24 に `wake34` の復旧が発動: `BT BTCORE-RELOAD rc=0`（再ダウンロード成功）→
  `STACK st=3/0` → `DISC rs=0x16` が**返ってきた**（＝チップがコマンドを受け付ける状態に戻った）。
  しかし電源 ON 後 `STACK st=2 (WORKING)` に到達せず `ADV-*` も出ないまま
  11:46:56 に `RECONN-TO` で dormant。以降 USB 出力モード
- **原因（推定）**: トランスポートを開いたまま（BTstack のデータソースがポーリング中に）
  `cyw43_bluetooth_hci_init()` でファームを再投入したため、その後の初期化シーケンスが
  完了しなかった
- **`wake36`**: 順序を修正。(1) 膠着時は `hci_power_control(HCI_POWER_OFF)` だけ投げる、
  (2) `HCI_STATE_OFF`（トランスポートが閉じた状態）で `cyw43_state.bt_loaded=false` にし、
  (3) `HCI_POWER_ON` → transport open がファームを再ダウンロード。
  ログは `BT FLOWCTL-RESET ctrl power-on reload=1`
  （`firmware/artifacts/remapper_anchor_wake36.uf2`, 11:48 ビルド）

## 2026-10-31 11:38 の BLE 断続切断は Mac のスリープ明け / OLED 段階ログを追加

- 10/30 22:28 に `wake34` を起動してから 13.2 時間、**切断は 2 回だけ**（11:38:12 と
  11:38:14、いずれも `rs=0x08` = 接続監視タイムアウト）。どちらも 0.35〜4 秒で自動再接続し、
  以降は安定（`lat` 最大 920ms は起床直後のみ）。**0x57 膠着（120 秒）ではない**
- 直前まで **4 時間以上アイドル**（最後の送信は 06:15 頃）。11:37:26 に `BT RESUME gap=3219s`
  → Mac のスリープ明けでコントローラがリンクを落とした、と判断（こちら側の制御では防げない）
- **OLED のスクリーンセーバー**: 仕様は「入力レポートが 60 秒来ないと減光、120 秒で
  バウンスロゴ」。ルディの観察は「**操作中に一瞬 1 行だけの表示**になる」なので、
  入力が来ているのに段階が進んでいる＝不具合の可能性あり。判定のため
  **段階遷移 (`OLED SS stage=0/1/2 idle=..s`) をログに出す**ようにした
  (`wake35` = `firmware/artifacts/remapper_anchor_wake35.uf2`, 11:42 ビルド)
- 現時点で**ポインタの飛び（A 型）は体感されていない**

## 2026-10-30 夜 CYW43 膠着が再発 / 復旧を「BT コア再投入」へ (`wake34`)

- 22:21:03 に Mac スロットへ接続 → 2 秒後に `CANSEND-TO` と
  `BIGDX dx=1504 dy=-716`（溜まった移動 2220px）→ `ok` が 139831 で凍結、
  `acl` が 90 → 696、`cp` も凍結（コントローラが完了通知を返さない）
- 22:23:03 に新設の `BT FLOWCTL-WEDGE ok_stall=120s acl=696 -> disconnect` が発動したが、
  **`DISC` が来ない** ＝ チップは disconnect コマンドすら処理しない。
  22:24:03 に再試行するも同じ。`m=0x1` のまま BLE は死んだ状態が続く
- ただし `BT XPORT ble=0` により出力は USB へフォールバック（Pico を USB 接続していれば
  入力だけは通る）
- **対策**: `cyw43_state.bt_loaded = false` → `cyw43_bluetooth_hci_init()` で
  **BT ファームだけ再ダウンロード**（Wi-Fi は無傷）→ `hci_power_control(OFF/ON)`。
  BT コアが作り直されれば HCI Reset が通り、`HCI_STATE_OFF` ハンドラから復帰できる
  （`wake34` = `firmware/artifacts/remapper_anchor_wake34.uf2`, 22:27 ビルド）
- **未検証**: 実機で膠着を再現させて `BT BTCORE-RELOAD rc=0` → `APPLY-DONE` → `ADV-EN`
  まで戻るか。rc≠0 や HCI_STATE_OFF に落ちない場合は次の手を検討

## 2026-10-30 ログ停止（Wi-Fi が `?` で固まる）— ファームの変更が原因ではない

- **症状**: `wake30` を焼いた直後から UDP ログが 1 通も届かない。OLED の Wi-Fi 欄が
  `.` → `*` → `?` を回り、`?` で止まって見える。ping は通る（39〜229ms、
  `192.168.2.250`）のでアソシエーション自体は生きている。リスナーは正常（自己テストで受信確認）
- **調査**: `wake30` と `wake28` の生成ソースを diff したところ、差分は
  `ble_hid.cc` の 0x57 膠着分岐**1 箇所だけ**（しきい値 120 秒・電源再投入→`gap_disconnect`）。
  この分岐は `current_state == BLE_STATE_CONNECTED` が前提で、**BT を切っている間は
  絶対に通らない**。よって Wi-Fi の `?` ループはファームの変更では説明できない
- **経過**: 10:21 に `wake30` を焼いて以降 15 分間ループ。10:37 に `wake28` へ戻すと即復帰
  （`WIFI JOINED t=12s`, rssi=-51, `rc=0`）。ただし復帰は「焼き直し」ではなく
  **時間経過（10:21→10:37）で AP 側の状態が解けた**可能性が高い
- **真因（10:55 判明）**: `firmware/src/remote_log_net.h` の **`RLOG_TARGET_IP` が
  誤って `192.168.215.1` に変更されていた**（mtime 10/30 01:15）。wake28 は 10/29 12:10
  ビルド＝古い（正しい）ヘッダ、wake29/30 は 10/30 10:1x ビルド＝新しいヘッダ。
  **コードは無罪で、ビルド間で Wi-Fi 設定だけが違っていた**
  - 見分け方: バイナリに `192.168.215.1` が含まれていたら誤設定（wake29/30/31/32 が該当）。
    wake28 には無い
  - **教訓**: 生成ソースの diff で secret ファイル (`remote_log_net.h`) を除外すると、
    まさにその差分を見落とす。設定ヘッダも「ビルド条件」として疑うこと
  - `?` ループの機序は未確定（宛先が別サブネットになり ARP/送信が空回りした影響とみる）
- **副産物**: インターネット共有を切ると Pico の **BLE まで止まる**ことを実測。
  CYW43 が Wi-Fi と BLE で同一 2.4GHz を共有しているため（再参加スキャンが BLE を餓死させる）
- **未着手**: ①再参加のバックオフを 3 秒固定から指数化、②リンク状態が NOIP でも
  静的 IP を持っていれば「接続中」とみなして leave しない、③`?` 滞留の番犬
- **復旧**: `RLOG_TARGET_IP` を 192.168.2.1 に戻し、修正コード入りの
  `firmware/artifacts/remapper_anchor_wake33.uf2`（10/30 10:54）をビルド。
  誤設定の wake29/30/31/32 は**焼かない**（`firmware/artifacts/DO_NOT_FLASH.txt` に記載）

## 2026-10-30 FLOWCTL-RESET のしきい値を 120 秒へ（ホストのスリープで誤爆 → BT 不通）

- **症状**: Windows→Mac へスロット切替すると、接続は成立するのに送信が止まり
  （`ok` が +8〜30 で凍結、`acl` が 3 秒で 25、10 秒で 337 まで増加）、
  `BLE STALL` の後 **FLOWCTL-RESET がコントローラを再起動 → Mac が再接続しない**
  （`RECONN-TO radio-off (dormant)` と `WAKE-RETRY` の繰り返し）。復旧は Pico の
  電源入れ直しのみ。**10/29 12:05・17:28、10/30 10:11 の 3 回とも同じ**
- **共通条件**: いずれも**Mac がスリープ/離席していた**時間帯（Windows 側で長時間
  作業した後の切替）。Mac が起きていた 10/30 00:32 の切替は 1.6 秒で正常接続
- **判断**: ホストがスリープ中でも LL 接続は成立し、HID 通知だけ引かれない。
  その間 0x57 で詰まるのは**待てば戻る**状態。コントローラ電源再投入は
  ホスト側の再接続を壊すだけで、3 回とも復旧に寄与しなかった
- **真因（10:17 判明）**: コントローラ電源再投入 (HCI_POWER_OFF/ON) の後、**CYW43 が
  HCI コマンドを受け付けなくなる**。BTstack 側は次のコマンドを投げるが、
  `ADV-PARAM` / `ADV-EN` の完了イベントが一度も返らず、**広告が出ないまま復旧不能**。
  10:11:44 以降のログには `APPLY-DONE` はあるが `ADV-*` が 1 行も無い。つまり
  「一度失敗すると以降どんなスロット切替でも繋がらない」＝ Pico の電源入れ直し以外に
  復旧手段が無い状態だった
- **修正**: FLOWCTL-RESET の電源再投入を**廃止**し、`gap_disconnect()` → 通常の再広告
  による復帰に変更（コントローラは生かしたまま）。しきい値 120 秒・接続 15 秒未満除外は
  据え置き、クールダウン 60 秒
- 検証ビルド `firmware/artifacts/remapper_anchor_wake30.uf2`（10/30 10:20）- **未解決**: Win→Mac 切替でホストがスリープだと詰まる件そのもの。対策案は
  「切替時にホストを起こす（運用）」「詰まり中は USB フォールバックへ落として
  リンクは保持」の 2 つ。後者は未実装

## 2026-10-28 夜間 soak は健全 / `gmax` が常に 0 = マウス判定の計器バグを修正

- **夜間 soak（`wake21`、10 時間 26 分・完全放置）**: `disc=0` / `blk=0` / `acl=0` /
  `dr=0`。`ok` は 6952→6977（放置中のボール微動のみ）。Wi-Fi は 5 回切断しすべて自動復帰
  （02:03 の 1 回だけ 2/4/8/16/32 秒リトライの末、BSSID 変更で復帰）
- **Mac のパニックは無関係**（`macreport.log`）: `mds_stores`（Spotlight）+
  `IOStorageFamily` + `lifs` の NULL 参照。BLE の kext は backtrace に無い。
  BOOTSEL のマスストレージを索引しようとした筋が濃いので `/Volumes` を
  Spotlight のプライバシーに追加して様子見（ファーム側の対処なし）
- **計器のバグ（2 段階で判明）**:
  - `25a10c4`: `in` がマウスとキーボードを合算していたため、打鍵間隔（100〜200ms）が
    `IHGAP` / `gmax` に「入力の穴」として出ていた（キーボード入力中にログが増える原因）
  - `029381c`: 次に「HID プロトコル == 2 (Mouse) ならマウス」で判定したが、この
    トラックボール (047D:2041) は `bInterfaceProtocol=0` で**一度も成立せず**、
    `gmax` が一晩中 0 のまま。`dense` も立たないので 3 種のスパイク行
    （`IHGAP`/`SENDGAP`/`LATGAP`）が全部沈黙していた ＝ **夜間の「スパイク 0」は
    健全の証拠にならない**
  - 修正: レポートデスクリプタの **Generic Desktop X (0x00010030)** でポインタ判定。
    マウント時に `ITF if=XXXX ptr=N` を 1 行出す。**5 秒以上の空白は穴に数えない**
    （放置後に触った 1 発目で `gmax` が跳ねるのを防ぐ）
- **`wake22`** (`firmware/artifacts/remapper_anchor_wake22.uf2`, 09:28 ビルド) に反映。
  検証ビルドは未焼き（帰宅後に焼く）
- **運用メモ**: Mac のパニック再起動後は `ssh-agent` の鍵が落ちて署名コミットが失敗する。
  `ssh-add --apple-use-keychain ~/.ssh/id_ed25519` で復帰。シェルに `SSH_AUTH_SOCK` が
  無い場合は `eval "$(ssh-agent -s)"`（launchd のソケットは
  `/private/tmp/com.apple.launchd.*/Listeners`、起動ごとに名前が変わる）

## 2026-10-26 遷移テスト（放置→再開 / BT スイッチ / スロット切替）は全て健全、観測の誤検知を 2 件修正

- **経緯**: 昨夜の一晩 soak（`wake6`、11 時間 40 分・無操作）は `DISC 0` で完走。
  ルディの観測「放置だけでは起きない。**触った / BT スイッチの後**が不安定」を受けて、
  遷移を意図的に踏むテストへ移行（`wake10` → `wake11`）
- **観測の追加**:
  - `BT RESUME gap=<N>s` (`c0480c2`): 10 秒以上の無送信のあと送信が再開した瞬間を 1 行。
    直後に `DISC` が出れば「再開が引き金」と確定できる
  - `BLE STALL` の閾値 5 秒 → **2 秒** (`c0480c2`): 一瞬の止まりも拾う
  - `OLED REINIT n=<n>` (`562c8ce`): I2C エラー再初期化の累計
- **OLED が上下反転で固まる症状** (`764aea8`):
  - 画像は正常なのに**上下だけ反転**、明るさ正常 = チャージポンプは生きているので
    コントローラのリセットではなく**設定コマンドの 1 つ（`0xC8` COM 走査方向など）の
    ビット化け**と判断（`0xC8`→`0xC0`, `0xDA 0x02`→`0x12` はいずれも 1 ビット差）
  - SSD1306 は I2C で読み出せず検証できないため、**1 秒ごとに設定を再表明**して
    最大 1 秒で自己修復させる（〜30 バイト/秒）
  - 実際に wake6 では「次の I2C エラーで再初期化されるまで反転が続いた」あと
    自然復旧した（`oled_bus_error` → `ssd1306_apply_init` の経路が効いた）
- **誤検知の修正** (`f516a0f`): BT OFF 中は入力が USB へ流れるのが正常なのに、
  それを「送れていない」と数えて再接続 0.3 秒後に `BLE STALL in=718/12s` を出していた。
  未接続中は判定しないようにした
- **Wi-Fi の `-2` 対策** (`1f33d9c`): 起動直後のスキャンが BLE と競合して
  `CYW43_LINK_NONET` になることがあり、30 秒固定待ちで復帰が 37 秒まで遅れた。
  NONET/FAIL は 2/4/8/16/32 秒で引き直す（BADAUTH は従来どおり 30 秒）
- **テスト結果（`wake10` / `wake11`）**:

  | 遷移 | 結果 |
  |---|---|
  | 66 分放置 → マウス操作 | OK（`BT RESUME gap=3967s`、`DISC` なし） |
  | スクリーンセーバー復帰（同 66 分放置） | OK（`OLED REINIT` 0、表示の化けなし） |
  | BT OFF → USB で使用 → ON（3 回、うち 95 秒） | OK（`ADV-EN`→`LE-CONN` 0.2 秒、`DISC rs=0x16`） |
  | スロット 1→2→1 | OK（`ADV-EN` の張り直し・再接続 1.2 秒、BL-32 再発なし） |

- **所感**: 今日は断続切断も「マウス止まり」も再現せず。放置 soak と遷移テストの
  両方で症状が出ないため、**再現条件はさらに別の要因**（Wi-Fi/ラジオ共存、ホスト側の
  スリープ等）の可能性。引き続き `BT RESUME` / `BLE STALL` / `DISC rs=` で監視する
- **ソーク用**: `remapper_anchor_wake6`（昨夜）/ `wake7` / `wake8` / `wake9` / `wake10` /
  `wake11`（最新・STALL 誤検知修正込み）。すべて `artifacts/` に保管

## 2026-10-25 Mac のインターネット共有が壊れて観測不能 → 復旧、OLED の `?`、Wi-Fi 再試行の短縮

- **観測経路が死んだ**: OLED は `*`（Wi-Fi JOINED）なのにログが届かない。
  原因は **Mac 側の Internet Sharing が「AP は立つがブリッジを作らない」**
  中途半端な状態（`ap1` は active、`member: ap1` のブリッジが存在せず、
  `com.apple.nat.plist` も消失）。CLI 復旧は SIP で不可 → **Mac 再起動で復旧**
- **学んだこと（運用メモ）**:
  - 共有ブリッジの**名前は再起動ごとに変わる**（`bridge102` → `bridge103`）。
    `bridge100/101/102` は VM (Docker/Colima の `vmenet`) 用なので混同しないこと
  - 正解の見つけ方: `member: ap1` を持ち `192.168.2.1` が付いたブリッジを探す
  - 共有の**サブネットは macOS が起動時に自動で選ぶ**（GUI にも plist にも設定が無い）。
    今回は 192.168.2.0/24 のままで助かったが、変わると Pico の静的 IP は無効になる
  - `ifconfig bridge102` に出た `192.168.215.0` は VM 側で、共有とは無関係だった
- **OLED の Wi-Fi 欄を 3 状態に** (`e9338d8`): `*`=JOINED かつ宛先 ARP 解決 /
  `?`=JOINED だが**宛先に届かない**（＝今日の状態）/ `!xx`=送信エラー。
  「繋がって見えるのにログが来ない」を画面で即断できるようにした
- **Wi-Fi 再試行の短縮** (`1f33d9c`): 起動直後のスキャンが BLE 接続とラジオを奪い合い
  `st=-2`（`CYW43_LINK_NONET`、OLED にも `-2` 表示）で失敗 → **30 秒固定待ち**で
  `t=4s` 失敗 → `t=37s` 接続となっていた。NONET/FAIL は **2/4/8/16/32 秒**で引き直す。
  BADAUTH (-3) は従来どおり 30 秒（連打しても悪化するだけ）
- **`wake5` で断続的な BT 切断**をルディが観測（ログは上の経路障害で取れず）。
  → `wake6` を焼いて観測再開。`wake5`/`wake6` の差は OLED `?` のみで BLE 側は同一
- **ソーク用**: `remapper_anchor_wake6.uf2`（観測中）/ `remapper_anchor_wake7.uf2`
  （wake6 + Wi-Fi 短バックオフ。次回焼き替え用）
- 参考: 断続切断の切り分けは `DISC rs=0x..`（HCI 理由コード）/ `BT FLOWCTL-RESET`
  （こちらの watchdog がコントローラ再起動）/ `BLE STALL` / `disc=` の増加を見る

## 2026-10-24 朝の USB-OUT と watchdog 暴走、休眠からの自動復帰

- **症状**: 朝起きたら USB-OUT（BLE 未接続）。夜間にホストがスリープして
  再接続タイムアウト → 電波 OFF（BL-01 の設計どおり）になっていた
- **そのうえ watchdog が暴走**していた: 09:18 に **1 秒おきに 4 回**コントローラを再起動
  - 原因: 「最後に通知が成功した時刻」を接続間で持ち越していたため、再接続した瞬間に
    「3 秒送れていない」と誤判定 → 即再起動 → また接続 → 再起動の連鎖
  - 修正 (`f568281`): **接続確立時に判定の基準を取り直す** + **再起動後 10 秒クールダウン**
- **休眠からの自動復帰を追加** (`904636d`, `727247d`): 再接続タイムアウトで電波を落とした
  状態を「休眠」として区別し、**入力イベントで 1 回だけ**接続を再試行する
  （ユーザー操作の BT-OFF は対象外。連続入力での再試行は 5 秒間隔に制限）
  - 最初の実装は 5 秒ごとに広告を張り直してしまい、`ADV-EN 0/1` が 5 秒周期で点滅 →
    ホストが接続のタイミングを掴めなかった。**1 休眠につき 1 回**に修正
- **macOS 側の切り分け（重要・運用メモ）**:
  - `ADV-EN en=1` は出る / `LE-CONN` が来ない / Mac の `SPBluetoothDataType` に
    `Remapper` のアドレス一致・RSSI あり・`Not Connected`
  - → **ファームは正常、macOS が接続要求を出していない**。Mac の Bluetooth を
    OFF→ON で復帰した（スリープ復帰後のスタック状態と思われる）
  - 逆に `ADV-EN` すら出ないときはファーム側（今日直した 3 つのどれか）
- ソーク用: `remapper_anchor_wake2.uf2`（11ms ペーシング / 0x57 復旧 + クールダウン /
  広告有効化の遅延 / USB 自動 re-attach / 休眠復帰 / `cp=`・`u=`・`sus=`）

## 2026-10-23 (2) 広告が二度と張られない不具合と、その修正

- **症状**: 一度接続したあと、スロット切替・RADIO-ON をしても `ADV-EN` が一切出ず、
  二度と接続できない（OLED は `Connecting...` で停止）。再起動するまで復旧しない
- **原因**: 接続するとコントローラは広告を止めるが、**BTstack は「広告有効」のまま**。
  その状態で `apply_slot_identity()` の `disable → enable` を同一瞬間に呼ぶと
  「変化なし」と判定され **HCI コマンドが送られない**。以降ずっと広告が出ない
- **修正** (`c8322dc`):
  1. 有効化を **200ms 遅延**（`adv_enable_pending`）して disable と打ち消し合わないように
  2. **切断完了時にも広告を張り直す**（接続中の enable は無視されるため）
  3. `HCI_STATE_WORKING` で明示的に `gap_advertisements_enable(0)`（`c28b83b`）
- **同時に判明したこと**:
  - 「BT-OFF で USB-OUT なのに効かない」は **USB が列挙されていない**だけだった
    （`u=0/0`。挿し直すと `u=1/1`）。切替ロジックは正常
  - 焼き直し後に Mac（ハブ）が再列挙に失敗して固まることがある → **未マウントが 3 秒続いたら
    D+ を落として再接続を促す**自動復帰を追加（`main.cc`、最大 5 回）
- **今日追加した観測**: `cp=<イベント数>/<完了パケット数>`（Number Of Completed Packets）、
  `u=<mounted>/<ready> sus=<0/1>`（USB 状態）。膠着時は `acl` 増・`ok`/`cp` 凍結を確認
- 11ms ペーシングで通知レートは 30 → 44〜85/s に改善（`gmax` も低下、`cp`≒`ok` でロスなし）
- ソーク用: `remapper_anchor_advfix.uf2`（11ms + 0x57 復旧 + USB re-attach + 広告修正）

## 2026-10-23 0x57 膠着の再発と自動復旧の修正

- 18:46 に **0x57 膠着が再発**（同日 2 回目）。スロット切替で再接続しても 2 秒で
  `CANSEND-TO` → `acl=90` と悪化し、`ok` は凍結したまま
- 追加した watchdog は正しく発火（`BT FLOWCTL-RESET ok_stall=3s acl=90` →
  HCI 電源 OFF→ON）したが、**復旧に失敗**:
  - 電源再投入後 `ADV-ADDR/PARAM/EN` が一切出ず、Mac が再接続できないまま
    30 秒の再接続タイムアウトで無線 OFF
  - 原因: **BTstack 側の広告状態キャッシュが「有効」のまま**で、`apply_slot_identity` の
    disable→enable が「変化なし」と判定され、HCI コマンドが送られていなかった
  - 修正 (`c28b83b`): `HCI_STATE_WORKING` で必ず `gap_advertisements_enable(0)` を呼び、
    状態変化を作ってから identity を適用する
- この間も**入力は USB-OUT にフォールバックして継続**（設計どおり）。ユーザー体感では
  「USB-OUT になっていた」
- 観測: 膠着中は `acl` が増え続け `ok`/`cp` が同時に凍結（`cp` も止まる）→ クレジットが
  コントローラ側から返っていない方向を支持
- ソーク用: `remapper_anchor_pace11_recover4.uf2`（11ms ペーシング + 復旧修正込み）

## 2026-10-22 (4) BL-45: アンカー確定 (`3a8d85b`)

- 10:42 ビルドの **`3a8d85b`**（`BL45_SKIP_ADDR_CHANGE` 無し = アドレス変更あり、
  `BLE_LOCK_DISABLE=ON`）を worktree で忠実に再ビルドし、実機で再確認した (14:08-14:09)
  - `PAIRING-START slot=1` → `ADV-ADDR/PARAM/EN` → `LE-CONN` → 接続
  - `SWITCH-REQ 1 -> 2` → `PAIRING-START slot=2` → `LE-CONN` (peer=00:72:EE:6E:C1:88)
  - **アドレスが `E4:...:27:31` → `E4:...:22:2F` と変わっているのに両方成功**
    → 「実行中のアドレス変更」自体は動作可能。回帰は別の変更が原因
  - 成果物: `firmware/artifacts/remapper_3a8d85b_rebuild.uf2`（目印: `BUILD VARIANT ble_lock=disabled`）
- **次回のビスect計画**（アンカーから 1 段ずつ、`BLE_LOCK_DISABLE=ON` を維持したまま）:
  1. `93d2546`（= b05bd98 アドレス値修正 + 3749b53 + リトライ条件変更を含む）
     を `BLE_LOCK_DISABLE=ON` でビルド → ここで NG なら原因はその 3 つのどれか
  2. OK なら `530766a`（ロックを送信経路のみに変更）で試す → ここで NG ならロック変更が原因
  3. 各段階でホストは Mac 固定・ボンド全消去・設定画面を開いたまま・60 秒待つ

## 2026-10-22 (3) BL-45: 参考実装 (EhHHKB2) との比較と、残った 3 差分

- ルディ提供の参考実装 `~/git/EhHHKB2` (RP2040+CYW43 のマルチペアリング HID) と比較。
  あちらは **ランダムアドレスを一切使わず**、`public` アドレス 1 本 +
  **accept list (filter policy 0x03)** + `ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION` で
  スロットを分離している。こちらの設計方針をそのまま採用した
- 判明した不足: うちの `btstack_config.h` に `ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION` が
  **無かった**（無いと `gap_load_resolving_list_from_le_device_db()` がリンクすらされず、
  RPA を使うホストを accept list で照合できない = ホワイトリスト方式が成立しない）。
  追加し、`MAX_NR_WHITELIST_ENTRIES` を 1 → 4 にした
- それでも**発見されない**ため、以下を順に切り分けた（すべて不可）:
  - public アドレスでの広告（`BT ADDR public=28:CD:C1:0E:D6:05` = CYW43 の正規アドレス）
  - accept list を使わない（policy 0 固定）
  - **最小構成の広告**（flags + `Remap-Test` の名前のみ / 100ms / policy 0）
  - コマンドは毎回 `ADV-PARAM` / `ADV-DATA` / `ADV-EN` すべて status 0x00
- **動いていた構成**: `ab02e0a`（`BL45_SKIP_ADDR_CHANGE=ON`、ランダムアドレスを
  起動時ラッチのまま使う・プライバシー解決なし・whitelist 操作なし）。12:24 / 13:19 に
  Mac/Windows で発見・ペアリング確認済み
- **残った差分は 3 つだけ**（次回はこれを 1 つずつ）:
  1. `ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION` の有無
  2. accept list 操作（`gap_whitelist_clear/add` と policy）の有無
  3. アドレス（起動時ラッチのランダム vs public）
- 次回の手順（統制）: アンカー `ab02e0a` を焼き直して**動くことを再確認** →
  上記 3 つを 1 つずつ足して都度確認。ホストは Mac 固定・ボンド全消去・設定画面を
  開いたまま・60 秒待つ。各ビルドの `BUILD` 行 (SHA) を必ず記録する
- soak 用の暫定版は `remapper_check_noaddr_ab02e0a.uf2`（全スロットが同一アドレス。
  別スロットのホストが接続要求してくる副作用あり）

## 2026-10-22 (続き) BL-45: 実行中のランダムアドレス変更で広告が電波に乗らない

- **症状**: ペアリングモードに入り `ADV-ADDR` / `ADV-PARAM` / `ADV-DATA` / `ADV-EN` が
  **すべて status 0x00** で通るのに、Mac / Android / Windows のどれからも発見されず
  `LE-CONN` も来ない。60 秒の広告期間ずっと同じ
- **切り分け結果** (同一コード + コンパイルスイッチ 1 個の A/B):
  | 条件 | 結果 |
  |---|---|
  | `BL45_SKIP_ADDR_CHANGE=ON`（実行中にアドレスを変えない） | **発見・ペアリング可** |
  | アドレス変更あり | **不可** |
- **棄却した仮説**（すべて「変更あり」で不可のまま）:
  - アドレス種別 (NRPA) 説 — 旧式の不正アドレスでも発見できていた。`b05bd98` の
    アドレス修正は仕様上は正しいが、今回の原因ではなかった
  - `ble_lock` 説 — 送信経路のみに絞っても不可（ペアリング中は送信しないので実質無ロック）
  - BL-43 の flow control 設定 — 無効化しても不可
  - Wi-Fi 共存 (`lwip_threadsafe_background`) — リモートログ無効ビルドでも不可
  - BTstack の `ENABLE_LE_SET_ADV_PARAMS_ON_RANDOM_ADDRESS_CHANGE` /
    明示的な `gap_advertisements_set_params` 再送 — 効かず
  - `hci_power_control(OFF/ON)` によるアドレス再設定 — 不可（チップのラッチは解除されない）
- **現時点の唯一の安定解**: アドレスを実行中に変えない（= 全スロットが起動時アドレスを共有）。
  `remapper_check_noaddr_ab02e0a.uf2` で Mac/Windows とも発見・ペアリング確認済み
- **次の候補**: (a) スロット切替時に Pico を再起動し、起動時ラッチの経路に乗せる
  （要: 切替先スロット + ペアリング意図の永続化）、(b) `3a8d85b` から 1 コミットずつ
  前進するビスect（当日の後半は複数要因が混ざっており、切り分けが濁った）
- 副産物: ビルド識別子 (`BUILD <時刻> <sha> [+dirty]`) と、`u=` (USB mounted/ready)、
  `gmax`/`lmax`/`lps`/`rtt` の観測が入った。焼き間違い・入力停止・ループ停止の切り分けが
  ログだけでできるようになった

## 2026-10-22 BL-45 再現: ペアリングモードなのに発見されない (Windows / Mac 両方)

- ルディ実機: Windows とペアリング不可。**Mac の「新規デバイス」画面にも出ない**ため、
  Windows 固有ではなく**広告が出ていない**ことが確定 (BL-45 の再現)
- ログ (`bl39_1809.log`) の状況: `PAIRING-START slot=2` → `APPLY-DONE st=1 pair=1`
  → **`LE-CONN` が一度も無い** (ホストが接続を試みていない)。試行のたびに
  `addr=` が変わる (ソルトローテーションは仕様どおり)
- なお**スロット 1 (Mac) が 5 時間動き続けたのは既存ボンドでの再接続**であり、
  発見 (スキャン) を必要としないため BL-45 の影響を受けない
- 仮説: `LE Set Advertise Enable` がコントローラに拒否されても (0x0C 等)
  ファームは「pair=1」と表示したまま無言で失敗する。`apply_slot_identity()` は
  `enable(0)` → アドレス設定 → `set_data` → `enable(1)` を数 µs で連続発行するため、
  直前コマンド未完了で `0x0C Command Disallowed` になり得る
- 対策と計装 (`a46e7eb`): 要求値と command-complete status を保持し
  `ADV-EN status=.. en=..` / `ADV-ADDR status=..` をログ、未受理なら
  PAIRING/RECONNECTING 中 1 秒間隔で `ADV-RETRY` として張り直す (自己修復)
- 次: 実機で `ADV-EN status` を確認 → `0x0C` なら原因確定、`0x00` なら
  アドバタイズパラメータ/アドレス側を追う

## 2026-10-21 BL-43/45 観測用: BLE ヘルスログ (BLE-HB / BLE STALL) + BL-04 セット移植

- **BL-04 後期セットを `feature/pico-w-ble` へ forward (`6ee0d59`)**: 実機検証済みの
  `b375cef`..`38c5666` を、現在の `ble_lock` ラッパーと BL-34 の Toast 流儀に合わせて移植。
  - ペアリング = `Ctrl+Win+Alt+Space` の 2 段階 (1 回目: カレントスロットのボンド消去 /
    2 回目: ペアリング開始)。`Ctrl+Win+Alt+Fx` はペアリングではなく OS へパススルー、
    切替は `Ctrl+Win+Fx` のみ
  - 切替・ON/OFF トグル受付から **3 秒ロック** (Toast `Can't switch now`)
  - ペアリングはカレントスロットのみ / `is_pairing_mode` と 60 秒カウントダウンは
    受付時点で開始し、150ms 遷移の適用時に再スタート
  - all-clear は保留キュー・リンクフラグ・復帰タイマを全リセット (切断ハンドラが
    clear 前の状態を参照しない)
  - スロット別ボンド消去 (`ble_clear_slot_bonds`) を追加
- **BLE ウェッジ観測 (`89381ef`)**: `ble_hid` に「受理した通知数 / 0x57 (ACL buffer full) /
  フロー制御待ちで捨てた数 / 切断回数と理由 / 最終通知時刻」のカウンタを追加し、
  `remote_log` が 1 秒周期で判定する。リアルタイム経路は加算のみ
  - `BLE STALL tx=6200ms ci=12 ok=1523 blk=0 acl=0 disc=1 rs=0x13` (接続中に 5 秒途絶)
  - `BLE STALL END ...` (復帰) / `BLE-HB ...` (15 秒ごと、変化が無ければ接続中のみ)
  - `blk` が増える = フロー制御待ち / `acl` が増える = コントローラ拒否 /
    `ok` が止まる = 通知自体が来ていない、と 3 分割で読める
- **焼き手順**: `git log --oneline -1` が `89381ef` 以降であることを確認してから
  `-DENABLE_REMOTE_LOG=ON` でビルド (secret は `src/remote_log_net.h`)。受信は
  `python3 tools/udp_log_listener.py -o <file>`。3 構成 (rlog on/off, BLE off) で
  ビルド確認済み・警告 0

## 2026-10-21 BL-39 完了: Wi-Fi UDP リモートログを `feature/pico-w-ble` へマージ

- **実機で安定動作を確認して完了**。切り分け用にルディが Mac をインターネット共有 AP
  (`bridge102` = 192.168.2.1) にして AiMesh から隔離した環境で、
  `seq`/`ack` が 1:1、`rtx=0`、`rc=0` の連続ログを確認 (起動直後の行も欠けない)
- **診断で嵌まった 2 点 (どちらも実装側で対処済み)**:
  1. 宛先 `RLOG_TARGET_IP` を更新し忘れると、lwIP は ARP 未解決でも `udp_send` に
     `ERR_OK` を返すため `se=0`・OLED `*` のまま無音になる → **ARP 解決状態を
     イベントで出す** (`RLOG target resolved/unresolved`) ようにした
  2. `ip4addr_ntoa()` は静的バッファを回すため、1 つの printf に複数回渡すと
     全部が最後の値になる (`STATIC IP ... gw=255.255.255.0 nm=255.255.255.0` の正体)
     → `ip4addr_ntoa_r()` に修正
- **実機要望の反映**: SSID は先頭 6 文字 + `*****` でマスク (証跡に残るため) /
  OLED の点滅ドットを状態条件なしに (main ループの生存確認を兼ねる) /
  ACK giveup を 10 秒 → 60 秒 / `rc=` (再接続・接続失敗の回数) を ALIVE と
  `WIFI LOST`/`WIFI FAIL` に / `se` を OLED に `!07` 形式で
- **マージ方針**: `bl39_remote_log` は分岐点が古く、`feature/pico-w-ble` 側が
  BLE 排他 (`ble_lock`)・BL-04 リファクタ・OLED (BL-34 スクリーンセーバー/Toast) で
  先行していたため、**コードは `feature/pico-w-ble` を正とし、BL-39 の追加分
  (remote_log 一式・イベント行・OLED Wi-Fi 欄・main フック) だけを載せた**。
  `bl39` 側にあった BL-04 の切替ロック (3 秒)・Space ペアリングホットキー・
  スロット別ボンド消去は、`feature/pico-w-ble` に未マージの別線 (`bl04_pairing_hotkey`)
  のため今回は載せていない (履歴としては `bl39_remote_log` に残る)。
  **注意**: BT 検出不可の原因はこの後期セットではなく、(1) BL-04 初期修正
  (`4e76652`/`f751492`: 広告の強制開始・all-clear で pairing 再開。pico-w-ble で
  `1cee7c5`/`2fb1e7e` として Revert 済み) と (2) `b089ac1` 以降の BL-43 マージ区間
  (BL-45・未特定) の 2 つ。後期セット (`b375cef` ほか) は実機で OK 確認済みなので、
  必要なら別ブランチで載せ直す
- **残課題**: 本番 AP (AiMesh) では BADAUTH (`st=-3`) で切断されるため、
  実環境でログを常用するにはルータ側の設定確認 (WPA2-Personal 固定 / PMF 無効 /
  バンドステアリング無効 / ノード固定) が必要。`rc=` で前後比較する

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
- **bring-up 修正群 (実機症状からの積み上げ)**:
  - `75cca84` STA モード起動漏れ — `cyw43_wifi_set_up`/`cyw43_arch_enable_sta_mode` を
    呼んでおらず link が DOWN のまま無音
  - `6720335` OLED WiFi インジケータ / `3caf43c` 失敗コード数字化 (`.` `*` `-1` `-3` `T<n>`)
  - `f8083f3` link 判定を `cyw43_wifi_link_status` → `cyw43_tcpip_link_status` へ
    (前者は関連付け後に JOIN 固定で DHCP 完了を検出できない罠)
  - `5f6138a` `dd7f2cb` ALIVE/WIFI TO と OLED の T コードに lwIP DHCP 状態を追加
    (state/tries: 2=INIT 6=SELECTING 1=REQUESTING 10=BOUND)。DHCP 無音時でも停止箇所が判る
  - `210afd7` 静的 IP opt-in (`RLOG_STATIC_IP/_GATEWAY/_NETMASK` の 3 点揃いで DHCP 停止)
  - **本コミット**: pbuf 確保を `cyw43_arch_lwip_begin/end` 内へ (RX 側 IRQ との mem ヒープ
    競合回避)、country 既定 0 罠の修正 (`#ifdef` が常に真 → 実行時判定に変更)、
    ALIVE/WIFI TO に `js` (wifi_join_state 生値) と `nf` (netif->flags) を追加、
    タイムアウト時に `cyw43_wifi_leave` で join を畳んでから再試行、
    lwipopts を pico-examples 既知良好構成へ (`LWIP_NETIF_TX_SINGLE_PBUF`, ACD/ARP check OFF)
- **現状**: 実機は「関連付け成功・DHCP 未完了 (旧 `T2` = `CYW43_LINK_NOIP`)」で停止。
  NOIP は `NETIF_FLAG_LINK_UP` が立っている証拠のため鍵交換は完了済み (KEYED 未完説は否定)。
  次手は静的 IP での経路確認と、`js`/`nf` 付き診断での DHCP 停止箇所の特定

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
## 2026-10-18 (BL-31 Pico 2 W 移行計画草案)

- **`BL31-PICO2W-PLAN.md` を新規作成** (ルディ指示: BL-31 方針確認を先倒し)。
  実装は未着手。要点:
  - submodule 実物調査の結果、**SDK 2.1.1 / TinyUSB (0.17.0 取り込み) / Pico-PIO-USB dev**
    がいずれも rp2350 対応 or 修正済み。upstream master (fork 分岐点前) に
    Waveshare RP2350-PiZero / USB-A / Flatbox rev8 の PIO-USB 動作実績コミットがあり、
    実装は CMake gate 緩和 + ボード定義が中心になる見込み
  - `remapper_single_rp2350.ld`, `src/main.cc` の `#if PICO_RP2350` (E10 errata 対応),
    CMake の `pico2` 分岐など土台も部分的に既存
  - **要検算**: RP2350 UF2 書込時の E10 末尾セクタ wipe と ble_hid.cc TLV
    (`FLASH_SIZE - 3*4096`) の干渉。必要なら config と同じ 1 セクタ下げ
  - 推奨方針: 同一 CMakeLists で pico_w / pico2_w 両対応 (方針 A)。ブランチ名 `bl31_pico2w`
- **ルディの部分回答を反映**:
  - **本命ターゲット = Waveshare Type-C 版** (入手難易度と Type-C のため)、
    次点 = 公式 Pico 2 W の Type-C 化。pico_w 純正対応は本家追従のため継続
  - ボード両対応のビルド仕分けで合意 (board header 分岐)
  - 注文メール型番に RP2350B 情報の断片 → **RP2350B (48 GPIO) の可能性**が出ている。
    公式 pico2_w.h (RP2350A 用) が直接使えず `src/boards/waveshare_*` 系専用 header
    追加が必要になる可能性。帰宅後に写真・URL・GPIO 本数・CYW43 有無を確認予定
  - soak test は用語説明済み (長時間連続稼働テストの意)

## 2026-10-18 (BL-43 P1 対策コミット)

- **BL-43 P1 対策を `bl43_bt_root_cause` ブランチにコミット (beeb02f)**:
  - `firmware/src/btstack_config.h`: 上流 pico-examples 準拠の CYW43 共有バス
    オーバーラン対策 — `MAX_NR_CONTROLLER_ACL_BUFFERS 3`,
    `MAX_NR_CONTROLLER_SCO_PACKETS 3`, `ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL`
    (`HCI_HOST_ACL_PACKET_LEN 1024` / `NUM 3` + SCO 必須ペア)。既知事例:
    6-8 回の接続/切断周期でスタック沈黙 (pico-sdk #2181 / btstack #654) —
    本現象の「切断ループ→電源再投入のみ復旧」と整合
  - `firmware/src/ble_hid.cc`: メインループ側の BTstack API 入口
    5 関数 (ble_switch_slot / ble_enter_pairing_mode / ble_clear_bonds /
    ble_hid_task / ble_hid_kick_reconnect) を ble_lock/unlock で包装。
    本体は `*_locked` ヘルパに分離。async context ロックは
    recursive_mutex 実装でありネスト・コールバック内再取得は安全なことを確認。
    TLV 遅延保存は save_slot_timer (非同期コンテキスト内) 実行なので追加ロック不要
  - ビルド検証: ENABLE_BLE=ON/OLED=ON (DP_PIN=2) と BLE-OFF スタブ構成で
    warning 0 / error 0
- **STATUS: 実機 soak test 待ち**。ループ再発消失を確認後 feature へマージ。
  再発する場合は P2 (reconnect チェーンへの 30s 降格 / RF・電源測定) に着手

## 2026-10-18 (OLED Line1 文字幅調整)

- **OLED Line1（各スロット状態）表示のはみ出し調整**:
  - `firmware/src/oled.cc`: Slot4 がクリップされないよう、固定幅(21 chars)想定のスペースを削減
  - ハウジング版で動作確認済（他不具合は soak 時に必要に応じて調整）

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
