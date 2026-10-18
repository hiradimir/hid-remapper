# BL-31: RP2350-W (Pico 2 W) 対応 — 移行計画草案

作成: 2026-10-18 / ステータス: **ル디のレビュー待ち** (このファイルは方針確認用・実装は未着手)

## 0. 結論サマリ

- 詰まる壁は思ったより少ない。**submodule と upstream コードが既にほぼ RP2350 対応済み**
- ピン・OLED・UART は公式 Pico 2 W で Pico W と電気的に共通。BLE (CYW43439/BTstack) も
  SDK 2.1.1 は pico_w / pico2_w 両方で同一ドライバ動作する
- 実装は主に **CMakeLists のチョコ 2-3 箇所とボード定義追加** から始まる。
  ビルドが通ったあと、BLE 起動 → ペアリング → dual host → 4 スロット soak の順に段階検証

---

## 1. 既に揃っている土台 (調査結果)

| 要素 | 状態 | 証拠 |
|------|------|------|
| pico-sdk 2.1.1 | RP2350 対応内包 (`src/rp2350/`、boards `pico2_w` 定義済) | submodule pinned bddd20f9 |
| TinyUSB (0.13.0-2847 = 0.17.0 取り込み済) | `hw/bsp/rp2040/family.cmake` に rp2350 platform 向け分岐あり | f77793517 |
| Pico-PIO-USB (dev 37965f8) | "Remove pio_sm_set_jmp_pin **Pico SDK 2 has it**" コミットで SDK 2.x 向け修正済 | 2024-10-25 |
| upstream hid-remapper (fork 分岐点前 master) | Waveshare RP2350-PiZero / RP2350-USB-A / Flatbox rev8 等 **rp2350 board 実績コミット多数** → PIO-USB が rp2350 で既に動作実績あり | 1f7ad5b, adf5ab9, cc3b6de |
| 単一ホスト用 RP2350 linker script | `remapper_single_rp2350.ld` 既存 (copy_to_ram 系ターゲット用。ENABLE_BLE = default binary では不要) | firmware/ |
| CMakeLists のボード条件 | `PICO_BOARD STREQUAL "pico2"` 分岐が UART/DP ピン定義まで存在 (pico2_w は未対応) | firmware/CMakeLists.txt L43, L48 |
| Config 用 RP2350 E10 errata 対応 | `src/main.cc` に `#if PICO_RP2350` 塊あり (末尾セクタ wipe 回避で config を 1 セクタ下げ) | src/main.cc L39-46 |
| SDK の RP2350 chip リビジョン対応 | `PICO_RP2350_A2_SUPPORTED` 既定 ON (A2+ の修正込み) | pico-sdk rp2350/pico_platform |

## 2. 実装タスク (推定の小ささ順)

1. **CMakeLists の gate 緩和**
   - `ENABLE_BLE requires PICO_BOARD=pico_w` を `pico_w OR pico2_w` 許可に変更
   - pico2_w 用の `PICO_DEFAULT_UART_TX/RX_PIN=16/17`、`PICO_DEFAULT_PIO_USB_DP_PIN=0`、
     `GPIO_VALID_PINS_BASE` 条件へ追加
   - `-DPICO_DUAL_PIO_USB_DP_PIN=2` はそのまま使える想定
2. **`PICO_BOARD=pico2_w ENABLE_BLE=ON` でまずビルドを通す** (コード自体は直さないつもりで。
   SDK が差分を多めに吸収)
3. **BLE 面の確認** (コード変更は無い前提で):
   - btstack_tlv / pairing flash bank: `PICO_FLASH_BANK_STORAGE_OFFSET=2084864`
     (2MB 手前) 位置はそのまま使える。`flash_size >= offset` assert は 4MB flash で成立
   - **ble_hid.cc の TLV (`TLV_FLASH_OFFSET = FLASH_SIZE - 3*4096`)** が pico2_w では
     4MB flash 末尾 3 セクタに移る。RP2350 UF2 書込時の **E10 errata による最終セクタ
     wipe** と接する端がどうなるか**要検算** (TLV の 3 セクタ内訳と erase/write 範囲の確認。
     必要なら main.cc の config と同じ発想で 1 セクタ下げ)
4. **コード面で RP2350 固有の残存差分確認** (`PICO_RP2350` マクロ差分の残像):
   - `__no_inline_not_in_flash_func` 等は SDK が共通化済み、XIP 位置も抽象化済み
   - `multicore_lockout` (Flash 書込時 Core 1 待機) は rp2350 でも同 API 存在
   - cyw43 / BLE 送受信は RP2040 と同一路
5. **Waveshare Type-C (需要が出た時点)**: 公式 `PICO_BOARD=pico2_w` では microUSB なので
   Type-C 版が必要なら `src/boards/` 配下の upstream パターン (waveshare_rp2350_xxx.h) を
   増やすだけ — 実装コスト最小
6. **BL-35 分離体制のまま** (`ble_hid.cc/h`, `oled.cc/h`) — platform 差のモジュール化不要と判定

## 3. 段階検証計画 (実機がある状態から)

| STEP | 内容 | 合格条件 |
|------|------|----------|
| 0 | `PICO_BOARD=pico2_w` で enabled 全構成のビルド | warning 0 |
| 1 | BLE 起動・1 スロットペアリング (Rudy の Mac) | HCI_WORKING, report 正常 |
| 2 | dual host (`PICO_DUAL_PIO_USB_DP_PIN=2`) + OLED 同時 | 2nd host と BLE が併走 |
| 3 | 4 スロット + soak (BL-43 適用版) | 切断ループ無し |
| 4 | ホットキー全套 + Web configurator / config 更新 | 全ホットキー動作 |
| 5 | BL-34/BL-40 (OLED 派生 branch) 同梱ビルド | 表示異常無し |

## 4. 選択肢・方針

- **方針 A (推奨)**: 同一 CMakeLists 内で pico_w / pico2_w を両対応にしていく
  (BL-35 の分離が済んでいるので相乗りコスト最低)
- **方針 B**: rp2350 専用の分岐ブランチに切り離す (コード量が予想より膨らんだ場合のみ)
- 開発ブランチ名: `bl31_pico2w`

## 5. ルディに確認したいこと (2026-10-18 追記: 部分回答済み)

**既に分かったこと (ルディ情報)**:
- 両方ある: 公式 Pico 2 W と Waveshare 互換品。**本命は Waveshare Type-C 版**
  (理由: 今後の入手難易度 + Type-C の存在)。次点は公式 Pico 2 W を Type-C 化する想定
- pico_w 純正対応を継続するのは upstream hid-remapper 本家への追従性のため
- **注文メールの型番表記に「RP2350」… とあり、着弾した Waveshare 基板は青色だった模様。**
  Blue Waveshare + Type-C + \*B\* チップ可能性 → **RP2350B (48 GPIO)** の可能性が出ている。
  RP2350B なら公式 `pico2_w.h` (RP2350A 30 pin 用) は使えず、
  `src/boards/waveshare_rp2350_pizero.h` (RP2350B=1, DP=28, GPIO 48 本) 系の
  Waveshare 専用 board header 追加が必須になる

**帰宅後の確認リスト (ルディ)**:
- [ ] 天板写真 (シルク文字・チップ刻印が見えるもの)
- [ ] Waveshare 製品ページ URL (または注文履歴リンク)
- [ ] Type-C 有無 + USB Host 用ピン配置 (GPIO0/1 か)
- [ ] WiFi/BLE (CYW43) 有無
- [ ] GPIO 本数 (30 か 48 か)

**未回答の質問**:
1. ~~ボード実物~~ → 上記チェックリストで確認後確定
2. **PICO_PLATFORM**: `rp2350` (arm Cortex-M33 系) で良いか (RP2350B なら DSP/M33 共通)。
   RISC-V (Hazard3) にする理由は無い認識
3. **優先順位確認**: BL-36 (切替遅延) / BL-41 (OS 連動 Modifiers) と BL-31 の手配順はどちらを先に?
   (ルディ発言からは BL-31 を本命にする意図と読むが確認したい)
4. ~~BL-43 soak test との絡み~~ → 用語説明済み。実運用 Pico W を観測的主体として継続し、
   Pico 2 W (Waveshare) で STEP1-5 を別枠進行でも問題ない認識 (確認してほしい)
