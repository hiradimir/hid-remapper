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
void ble_hid_send_report(uint8_t report_id, const uint8_t* data, uint8_t len);

void ble_pair_new_device(void);
void ble_clear_bonds(void);

#ifdef __cplusplus
}
#endif

#endif
