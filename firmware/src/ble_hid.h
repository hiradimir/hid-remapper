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
    uint32_t disc;       // 切断イベントの累計
    uint8_t disc_reason; // 直近の HCI 切断理由
    uint64_t last_ok_us; // 最後に受理された通知の時刻 (0 = まだ)
} ble_hid_health_t;

#if defined(ENABLE_BLE) && ENABLE_BLE

void ble_hid_init(void);
void ble_hid_task(void);
bool ble_hid_is_connected(void);
// 接続中でも 0x57 枯渇で送信が死んでいる場合は true (USB フォールバック判断用)
bool ble_hid_is_tx_stalled(void);
// ペアリングモード (広告中/待機) かどうか。Wi-Fi の一時停止判断に使う
bool ble_hid_is_pairing(void);
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
void usb_clear_kbd_report(void);

#else

// ENABLE_BLE=OFF 時のインラインスタブ（呼び出し側のオーバーヘッドを完全にゼロ化）
static inline void ble_hid_init(void) {}
static inline void ble_hid_task(void) {}
static inline bool ble_hid_is_connected(void) { return false; }
static inline bool ble_hid_is_tx_stalled(void) { return false; }
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
