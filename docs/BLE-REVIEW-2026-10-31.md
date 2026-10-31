# BLE HID 不安定事象のレビューと即時対処

- 日付: 2026-10-31
- 対象: `hiradimir/hid-remapper` / `feature/pico-w-ble`
- 対象コミット: `8af20f4`
- 実機ビルド: `PICO_BOARD=pico_w`、`ENABLE_BLE=ON`、`ENABLE_OLED=ON`、`ENABLE_REMOTE_LOG=ON`、`BLE_LOCK_DISABLE=ON`
- 参考ログ:
  - `projects/p-hid-remapper/logs/bl39_1025_2240.log`
  - `projects/p-hid-remapper/logs/bl39_1029_1732.log`
  - `projects/p-hid-remapper/logs/bl39_1030_1055.log`
  - 補助: `bl39_1030_1021.log` / `bl39_1030_1046.log`

> `BLE_LOCK_DISABLE=ON` は BL-45 の切り分け用。常用ビルドでは OFF（`ble_lock` 有効）に戻す想定。
> ログはこの切り分けビルドのものだが、0x57 は `ble_lock` 無効でも発生している。

---

## 0. 要旨

**「実装方向の誤りは見当たらない」とは言えない。** ただし「設計全体が間違い」でもない。

コードとログで裏が取れている問題は次の3点。

1. **0x57（ACL バッファ満杯）の監視が構造的に発火しない**
   - 0x57 のたびに監視タイマーを張り直すため、連続 0x57 を検出できない
   - 10/30 の「再起動まで復旧しない」と一致
2. **送信結果が上位に伝わらない**
   - 送信できなくてもキューから pop され、破棄が観測できない
3. **接続パラメータの要求値が Apple の一般則から外れている**
   - 送信レート低下の有力仮説。ただし実際の latency をログに出しておらず未証明

「飛び飛び」は **遅延でもランダムな飛びでもなく、単位時間あたりのポインタ位置（= BLE 送信レート）の低下**。

---

## 1. 症状

- マウスで円軌道を描くと、**単位時間あたりのポインタ位置の数が減る**
- 起動直後は位置数が多いが、時間経過で減る
- 併発事象:
  - BLE スロット切替で接続できなくなる
  - Windows でペアリングできない
  - 時間経過で disconnect を繰り返す（最近は少し落ち着いている）

---

## 2. ログの読み方（3つのシグネチャ）

| ログ | 観測 | 機序（現時点の整理） |
|---|---|---|
| **10/30** | 13:59 に 0x57 が 549 回（`acl 115→664`）、`ok`/`cp` 凍結、`STALL END tx=20766ms`、再起動まで復旧せず | **0x57 ループ + 監視タイマー再アーム**。`CANSEND-TO` / `FLOWCTL-DEADLOCK` は出ていない |
| **10/25** | `acl=0` / `blk=0`、`rs=0x08`（supervision timeout）、12 秒ストール | 0x57 ではない。相手無応答の別経路 |
| **10/29** | 0x57 なし、`SENDGAP` 1649 件（p50 314ms / p95 914ms） | 送信間隔の広がり。CI 待ち / キュー詰まり / 合流の別経路 |

**同じ「接続不良」としてまとめない方がよい。** 少なくとも 10/30 と 10/25 は別の層。

補足:
- `BLE-HB` は 15 秒ごと。`ok` は累積送信成功、`in` は前回 HB からの入力数。
- `lat` は「CAN_SEND_NOW を要求してから送信するまで」の最大待ち。connection latency そのものではない。
- `clat`（実際の connection latency）は保存されているが、**HB に出ていない**。

---

## 3. 確定した問題

### 3.1 0x57 監視の再アーム

- `firmware/src/ble_hid.cc:2010-2019`
  - `status == 0x57` のたびに `remove_timer → set_timer(2000) → add_timer`
  - `ble_can_send = false` にして `hids_device_request_can_send_now_event()` を再要求
- `firmware/src/ble_hid.cc:427-450`
  - `can_send_timeout_handler` は**タイマーが発火したときだけ** `consecutive_can_send_timeouts++`
  - 2 回連続で `FLOWCTL-DEADLOCK force-disc`
- `firmware/src/ble_hid.cc:1288`
  - `CAN_SEND_NOW` で `consecutive_can_send_timeouts = 0`、`ble_can_send = true`、タイマー remove

**結果**: 0x57 が 2 秒未満で続く限り、監視は一度も発火しない。10/30 のログに `CANSEND-TO` / `FLOWCTL-DEADLOCK` が無いことと一致する。

### 3.2 送信結果の伝播

- `firmware/src/ble_hid.cc:1966` `ble_hid_send_report` は `void`
- `!ble_can_send` のとき `health_blocked++` して return（`:1978-1982`）
- 呼び出し元の `do_send_report_ble` は**常に true を返す**（`:2373`）
- `firmware/src/remapper.cc:1448-1461` は `sent=false` でも pop する
  - 実装コメント: `// XXX even if not sent?`

**結果**: 送信できなかったレポートがキューから消え、破棄が上位に伝わらない。マウスは入力段で相対移動を合流しているため実害は限定的だが、キーボード / consumer のエッジは落ちうる。

### 3.3 接続パラメータの要求値

- `firmware/src/ble_hid.cc:1416-1443`
  - `HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE` で `interval` / `latency` / `timeout` を保存
  - `latency > 0` なら 10 秒おきに `gap_request_connection_parameter_update(con_handle, 9, 12, 0, 400)`
  - 11.25〜15ms / latency 0 / 4 秒
- Apple の一般則:
  - `Interval Min >= 15ms`
  - `Interval Min` は 15ms の倍数
  - `Interval Min + 15ms <= Interval Max`（または `Min = Max = 15ms`）
  - HID 例外は「11.25ms まで受け入れられうる」だが、`Min = Max = 11.25ms` の形が想定
- 現在の `(9, 12)` は `min + 15 > max` で一般則から外れる

**仮説**: macOS が latency 4 を維持すると、実効 15ms × 5 = 75ms → **13.3/s**。実測 `ok/s` 7〜14 と整合する。ただし `clat` がログに無いため未証明。

---

## 4. 即時対処（優先順）

### P0: 観測のみ（今日、挙動変更なし）

目的: 送信レート低下の原因を、推測ではなく数値で確定する。

- `BLE-HB` に次を追加する
  - `clat=%u` — 実際に適用されている connection latency
  - `st=%u` — supervision timeout
  - `cu=%u` — `HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE` の受信回数（10 秒再要求のフラップ検出）
  - `ori=%u` — 送信待ちの `or_items`
  - `sent` / `drop` / `0x57` の内訳
- 1〜2 時間の円軌道テストを 1 本取り、`ok/s` / `in/s` / `lat` / `clat` / `lmax` / `lps` を時系列で並べる

**リスク**: なし。**受け入れ基準**: `clat` と `ok/s` の相関が見えること。

### P1: 0x57 監視の修正（今日〜今週）

- 0x57 では**タイマーを再アームしない**（最初の失敗時刻をラッチ）
- 0x57 の連続回数と経過時間を**直接カウント**（タイマー発火に依存しない）
- N 回連続、または T 秒経過で「送信停止 → 必要なら `gap_disconnect` → 再広告」
- `CAN_SEND_NOW` でのリセットは「送信成功」時だけに限定
- `ACLFULL n=.. dur=..` をログに出す

**リスク**: 挙動変更。強制切断の閾値が短すぎると、ホスト睡眠中に不要な切断が増える。
**受け入れ基準**: 10/30 と同じ状況で再起動不要、または N 秒以内に自動復帰。通常使用で切断が増えない。

### P1: 送信結果の伝播（今週）

- `ble_hid_send_report` を `bool` にし、成功/失敗を返す
- `do_send_report_ble` も結果を伝播
- `send_report` は**成功時だけ pop**（失敗時は最新状態を保持）
- マウスは最新値の合流、キーボード / consumer はエッジを保持
- `drop` / `sent` / `0x57` をカウンタで出す

**リスク**: キューが詰まる可能性。
**受け入れ基準**: キーボードの取りこぼしがなく、キューが無限に増えない。`drop` がログで説明できる。

### P2: 接続パラメータの A/B（今週）

- 要求値を `(12, 12, 0, 400)`（15ms）または `(9, 9, 0, 400)`（11.25ms）に変える
- 要求は**接続時 1 回**、または latency が変わったときだけにする。10 秒周期の再要求は外す
- `clat` / `cu` をログに出し、受理 / 拒否 / フラップを確認
- 受理されない場合、13.3/s を上限として認めるか、interval 側で再交渉するかを決める

**リスク**: ホスト側の省電力設定と競合する。
**受け入れ基準**: `clat=0` が維持され、`ok/s` が 50/s 以上に改善。または受理されないことが明確になる。

### P2: 送信ポリシー / 一時的なレート制限（今週〜）

- マウスは「最新の移動量を 1 件」に合流して送る
- キーボード / consumer は合流しない
- 0x57 が続く間は送信を止める、または 30〜40/s に制限する
- ただし今回の 0x57 は「ホストが credit を返さない」ことが主に見えるため、レート制限は悪化を遅らせる対処。根本対策は P1

**リスク**: レート制限でさらに位置数が減る可能性。
**受け入れ基準**: 0x57 の頻度が下がり、通常操作の `ok/s` を不必要に落とさない。

### P3: 4 スロットの identity / bond 設計（後続）

- 4 スロットが public BD_ADDR 1 本、`le_device_db` 4 エントリ共有、slot→index 対応なし
- 非ペアリング時のスロット検査なし、暗号化成功で無条件確定
- ホストから見ると全スロットが同一デバイス。Windows のペアリング失敗 / スロット切替の不安定さと関連する**設計リスク**だが、実害は未検証
- 即時修正ではなく、製品仕様の判断（「1 台に見せて中で切替」か「4 台に見せる」か）が必要
- 当面の運用: 1 ホスト 1 スロットを守る、切替前にホストを起こす、必要なら bond を消す

---

## 5. やらない方がよいこと

- **0x57 でコントローラを電源再投入しない**
  - `235aa79` で廃止済み。3 回実測で復旧せず、ホスト側の再接続を壊す
- **`BLE_LOCK_DISABLE=ON` を常用ビルドにしない**
  - BL-45 の切り分け用。ログはこの変種だが、0x57 は lock 無効でも起きている
- **`HCI_ACL_PAYLOAD_SIZE` / `HCI_HOST_ACL_PACKET_LEN` を闇雲に下げない**
  - btstack #654 と同型の値だが、本件での A/B は未実施
- **HISTORY の「インターネット共有を切ると BLE が止まる」を原因として扱わない**
  - 設定ミス由来の同時観測。因果は未確定
- **10/25 / 10/29 / 10/30 を同じ原因としてまとめない**
  - 10/30: 0x57 ループ
  - 10/25: `rs=0x08`（supervision timeout）+ 12 秒ストール
  - 10/29: 0x57 なしの `SENDGAP` 1649 件

---

## 6. 検証と受け入れ基準

| 項目 | 検証 | 受け入れ基準 |
|---|---|---|
| P0 観測 | 1〜2 時間の円軌道テスト | `clat` / `ok/s` / `lat` の相関が見える |
| P1 0x57 | 10/30 と同じ状況を再現 | 再起動なしで復帰、または N 秒以内に自動復帰。通常使用で切断が増えない |
| P1 送信結果 | キーボード / マウスの連続入力 | 取りこぼしがない。`drop` が説明できる。キューが増え続けない |
| P2 接続パラメータ | `(12,12,0,400)` / `(9,9,0,400)` の A/B | `clat=0` が維持され、`ok/s` が改善。`cu` が 10 秒周期で増えない |
| P3 identity | 別スロットの bond / Windows 再ペアリング | 別スロットの bond で暗号化が成立しない。Windows で再ペアリングできる |

---

## 7. 現時点で言えること / 言えないこと

**言えること**

- 10/30 の「再起動まで復旧しない」は、0x57 のたびに監視タイマーを再アームする構造と、コード・ログの両方で一致する
- 送信結果が上位に伝わらず、失敗しても pop するため、破棄が観測できない
- 接続パラメータの要求値は Apple の一般則から外れており、Mac が latency 4 を維持すると実効 13.3/s になる。実測 `ok/s` 7〜14 と整合する

**言えないこと**

- `clat` の実値（latency 4 が実際に維持されているか）
- `(9,12,0,400)` が macOS に受理されているか
- 7/s の下限、RLOG の寄与、0x57 と latency の関係
- 「これが飛び飛びの主因」という断定

---

## 8. 推奨する進め方

1. **今日**: P0 の観測追加（`clat` / `st` / `cu` / `ori` / 送信結果 / 0x57 連続回数）を入れて、円軌道テストを 1 本取る
2. **今週**: P1 の 0x57 監視修正と送信結果の伝播を実装し、同じテストで比較
3. **次**: P2 の接続パラメータ A/B（`12,12,0,400` / `9,9,0,400`）と、10 秒再要求の停止
4. **その後**: 必要なら RLOG=OFF（UART）での A/B、4 スロット identity の設計判断

---

## 9. 参考: 主な file:line

- `firmware/src/ble_hid.cc:427-450` — `can_send_timeout_handler`
- `firmware/src/ble_hid.cc:1288` — `CAN_SEND_NOW` でのカウンタリセット
- `firmware/src/ble_hid.cc:1416-1443` — 接続パラメータの保存と再要求
- `firmware/src/ble_hid.cc:1966-2035` — 送信パスと 0x57 分岐
- `firmware/src/ble_hid.cc:2373` — `do_send_report_ble` が常に true
- `firmware/src/ble_hid.cc:2398-2429` — `ble_transport_task` の送信ゲート / pace
- `firmware/src/remapper.cc:1448-1461` — `send_report` の pop
- `firmware/src/remote_log.cc:992` — `BLE-HB` フォーマット
- `firmware/src/remote_log.cc:849` — HB は 15 秒周期
