#ifndef _BLE_HID_H_
#define _BLE_HID_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif
