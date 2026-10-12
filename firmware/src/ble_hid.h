#ifndef _BLE_HID_H_
#define _BLE_HID_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*ble_send_report_fn_t)(uint8_t interface, const uint8_t* report_with_id, uint8_t len);

#if defined(ENABLE_BLE) && ENABLE_BLE

void ble_hid_init(void);
void ble_hid_task(void);
bool ble_hid_is_connected(void);
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
static inline bool ble_hid_check_hotkey(uint8_t interface, const uint8_t* report_with_id, uint8_t len) {
    (void)interface; (void)report_with_id; (void)len;
    return false;
}

#endif

#ifdef __cplusplus
}
#endif

#endif
