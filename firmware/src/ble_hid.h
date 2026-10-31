#ifndef _BLE_HID_H_
#define _BLE_HID_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*ble_send_report_fn_t)(uint8_t interface, const uint8_t* report_with_id, uint8_t len);

// BLE ウェッジ観測用のヘルスカウンタ (BL-43/45)。
// リアルタイム経路ではカウンタを増やすだけで、整形はメインループ側で行う
typedef struct {
    uint16_t ci;         // 接続間隔 (1.25ms 単位、0 = 未接続)
    uint32_t ok;         // HCI が受理した通知の累計
    uint32_t acl_full;   // 0x57 (BTSTACK_ACL_BUFFERS_FULL) の累計
    uint32_t blocked;    // ble_can_send 待ちで捨てたレポート累計
    uint32_t err;        // 0x57 以外の送信エラー累計
    uint32_t cp_events;  // Number Of Completed Packets イベントの累計
    uint32_t cp_packets; // その中で報告された完了パケット総数
    uint32_t disc;       // 切断イベントの累計
    // 直前の送信から 2ms 未満で送った回数 (接続イベントへの詰め込み指標)。
    // 15ms 間隔のリンクで 2 個以上を 1 イベントに入れるとホスト側で
    // 「止まる→まとめて動く」に見えるため、その発生量を数える
    uint32_t burst;
    // 「BTstack に渡してから完了通知が返るまで」の最大値 (ms)。
    // ここが大きい = 電波に乗るまでの待ちが長い (LL 再送・接続イベント待ち)
    uint32_t lat_max_ms;
    // 1 レポートに載った移動量の最大値 (|dx|+|dy|)。送信が細切れにならず
    // 「溜めて 1 発で送った」瞬間はここが跳ねる = カーソルが飛ぶ瞬間
    uint32_t dmax;
    // 送信が「黙って」捨てられた理由の累計。ok が伸びないのに blk/acl/err も
    // 動かないとき、どの早期 return に落ちているかを特定する
    uint32_t sk_state; // 接続状態でない
    uint32_t sk_enc;   // 未暗号化
    uint32_t sk_ready; // レポート未購読 (CCCD)
    // bit0 接続 / bit1 暗号化 / bit2 mouse ready / bit3 kbd ready /
    // bit4 consumer ready / bit5 tx_stalled / bit6 dormant
    uint8_t flags;
    uint8_t disc_reason; // 直近の HCI 切断理由
    // 2026-10-31 追加: 「送信レートが落ちる」原因を推測でなく数値で見るため
    uint16_t clat;       // 実際に適用されている connection latency
    uint16_t st;         // supervision timeout (10ms 単位)
    uint32_t cu;         // Connection Update Complete の受信回数 (再要求のフラップ検出)
    uint32_t acl_run;    // 連続 0x57 の回数 (成功で 0 に戻る)
    uint64_t last_ok_us; // 最後に受理された通知の時刻 (0 = まだ)
} ble_hid_health_t;

#if defined(ENABLE_BLE) && ENABLE_BLE

void ble_hid_init(void);
void ble_hid_task(void);
bool ble_hid_is_connected(void);
// 接続中でも 0x57 枯渇で送信が死んでいる場合は true (USB フォールバック判断用)
bool ble_hid_is_tx_stalled(void);
// 再接続タイムアウトで電波OFF (休眠) 中か
bool ble_hid_is_dormant(void);
// 休眠から入力契機で接続を試みる
void ble_hid_wake_from_dormant(void);
// ペアリングモード (広告中/待機) かどうか。Wi-Fi の一時停止判断に使う
bool ble_hid_is_pairing(void);
// 送信レイテンシ最大値をリセット (観測ウィンドウの区切りで remote_log が呼ぶ)
void ble_hid_reset_lat_max(void);
void ble_hid_reset_dmax(void);
bool ble_hid_can_send(void);
void ble_hid_send_report(uint8_t report_id, const uint8_t* data, uint8_t len);
void ble_hid_kick_reconnect(void);

void ble_pair_new_device(void);
void ble_clear_bonds(void);

void ble_enter_pairing_mode(int slot);
void ble_switch_slot(int slot);
void ble_hid_switch_host(uint8_t slot);

void ble_led(bool value);
uint16_t ble_hid_get_conn_interval(void);
uint32_t ble_hid_get_connected_slot_mask(void);
void ble_hid_get_health(ble_hid_health_t* out);

// ホットキー判定（スロット切替・ペアリングモード・ボンド消去）
bool ble_hid_check_hotkey(uint8_t interface, const uint8_t* report_with_id, uint8_t len);

// BLE/USB/オフラインのトランスポート自動ディスパッチ・送信タスク
void ble_transport_task(ble_send_report_fn_t do_send_usb);

// USB 側ヘルパー（main.cc 側で TinyUSB を直接操作）
bool usb_is_mounted(void);
bool usb_is_ready(void);
bool usb_is_suspended(void);
void usb_clear_kbd_report(void);

#else

// ENABLE_BLE=OFF 時のインラインスタブ（呼び出し側のオーバーヘッドを完全にゼロ化）
static inline void ble_hid_init(void) {}
static inline void ble_hid_task(void) {}
static inline bool ble_hid_is_connected(void) { return false; }
static inline bool ble_hid_is_tx_stalled(void) { return false; }
static inline bool ble_hid_is_dormant(void) { return false; }
static inline void ble_hid_wake_from_dormant(void) {}
static inline bool ble_hid_is_pairing(void) { return false; }
static inline bool ble_hid_can_send(void) { return false; }
static inline void ble_hid_send_report(uint8_t report_id, const uint8_t* data, uint8_t len) {
    (void)report_id; (void)data; (void)len;
}
static inline void ble_hid_kick_reconnect(void) {}
static inline void ble_pair_new_device(void) {}
static inline void ble_clear_bonds(void) {}
static inline void ble_enter_pairing_mode(int slot) { (void)slot; }
static inline void ble_switch_slot(int slot) { (void)slot; }
static inline void ble_hid_switch_host(uint8_t slot) { (void)slot; }
static inline void ble_led(bool value) { (void)value; }
static inline uint16_t ble_hid_get_conn_interval(void) { return 0; }
static inline uint32_t ble_hid_get_connected_slot_mask(void) { return 0; }
static inline void ble_hid_get_health(ble_hid_health_t* out) {
    if (out == nullptr) return;
    out->ci = 0;
    out->ok = 0;
    out->acl_full = 0;
    out->blocked = 0;
    out->err = 0;
    out->cp_events = 0;
    out->cp_packets = 0;
    out->disc = 0;
    out->disc_reason = 0;
    out->last_ok_us = 0;
}
static inline bool ble_hid_check_hotkey(uint8_t interface, const uint8_t* report_with_id, uint8_t len) {
    (void)interface; (void)report_with_id; (void)len;
    return false;
}

#endif

#ifdef __cplusplus
}
#endif

#endif
