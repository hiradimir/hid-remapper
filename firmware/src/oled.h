#ifndef OLED_H
#define OLED_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OLED_BT_DISCONNECTED = 0,
    OLED_BT_CONNECTED    = 1,
    OLED_BT_PAIRING      = 2,
    OLED_BT_SWITCHING    = 3,
} oled_bt_state_t;

#if defined(ENABLE_OLED) && ENABLE_OLED

void oled_init(void);
void oled_task(void);

// 状態更新関数（スレッドセーフ：Core 0 から随時更新可能）
void oled_set_mode(bool ble_mode); // true: BLE, false: USB
void oled_set_slot(uint8_t slot);
void oled_set_bt_state(oled_bt_state_t state);
void oled_set_slot_bonded(uint8_t slot, bool bonded);
void oled_set_slot_os(uint8_t slot, char os_char);
void oled_set_usb_port(uint8_t port, const char* dev_str); // port: 0 (P1), 1 (P2), dev_str: "KBD", "MS ", "DEV", "---"
void oled_set_status(const char* status_msg);
void oled_set_peer_addr(const char* peer_addr_str);
void oled_notify_tx(void);
void oled_notify_activity(void);
void oled_show_toast(const char* toast_msg, uint32_t duration_ms);
void oled_set_radio_off(bool is_off);
void oled_set_pairing_remaining_sec(int sec);
void oled_set_reconnect_remaining_sec(int sec);

// W-01/W-17: USBポートのデバイス種別を定期判定し表示更新
void oled_update_usb_ports(void);

#else

// ENABLE_OLED=OFF 時のインラインスタブ（呼び出し側のオーバーヘッドを完全にゼロ化）
static inline void oled_init(void) {}
static inline void oled_task(void) {}
static inline void oled_set_mode(bool ble_mode) { (void)ble_mode; }
static inline void oled_set_slot(uint8_t slot) { (void)slot; }
static inline void oled_set_bt_state(oled_bt_state_t state) { (void)state; }
static inline void oled_set_slot_bonded(uint8_t slot, bool bonded) { (void)slot; (void)bonded; }
static inline void oled_set_slot_os(uint8_t slot, char os_char) { (void)slot; (void)os_char; }
static inline void oled_set_usb_port(uint8_t port, const char* dev_str) { (void)port; (void)dev_str; }
static inline void oled_set_status(const char* status_msg) { (void)status_msg; }
static inline void oled_set_peer_addr(const char* peer_addr_str) { (void)peer_addr_str; }
static inline void oled_notify_tx(void) {}
static inline void oled_notify_activity(void) {}
static inline void oled_show_toast(const char* toast_msg, uint32_t duration_ms) { (void)toast_msg; (void)duration_ms; }
static inline void oled_set_radio_off(bool is_off) { (void)is_off; }
static inline void oled_set_pairing_remaining_sec(int sec) { (void)sec; }
static inline void oled_set_reconnect_remaining_sec(int sec) { (void)sec; }
static inline void oled_update_usb_ports(void) {}

#endif

#ifdef __cplusplus
}
#endif

#endif // OLED_H
