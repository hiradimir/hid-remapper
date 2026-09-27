#if ENABLE_BLE

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "btstack.h"
#include "ble/le_device_db.h"
#include "ble_mouse.h"
#include "ble_hid.h"
#include "ble/sm.h"

static const uint8_t hid_report_descriptor[] = {
    // --- Mouse (Report ID 1) ---
    0x05, 0x01,                    // Usage Page (Generic Desktop Ctrls)
    0x09, 0x02,                    // Usage (Mouse)
    0xA1, 0x01,                    // Collection (Application)
    0x85, 0x01,                    //   Report ID (1)
    0x09, 0x01,                    //   Usage (Pointer)
    0xA1, 0x00,                    //   Collection (Physical)

    // Buttons (8 buttons = 1 byte)
    0x05, 0x09,                    //     Usage Page (Button)
    0x19, 0x01,                    //     Usage Minimum (0x01)
    0x29, 0x08,                    //     Usage Maximum (0x08)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x25, 0x01,                    //     Logical Maximum (1)
    0x95, 0x08,                    //     Report Count (8)
    0x75, 0x01,                    //     Report Size (1)
    0x81, 0x02,                    //     Input (Data,Var,Abs)

    // X, Y (16-bit each = 4 bytes)
    0x05, 0x01,                    //     Usage Page (Generic Desktop Ctrls)
    0x09, 0x30,                    //     Usage (X)
    0x09, 0x31,                    //     Usage (Y)
    0x16, 0x00, 0x80,              //     Logical Minimum (-32768)
    0x26, 0xFF, 0x7F,              //     Logical Maximum (32767)
    0x75, 0x10,                    //     Report Size (16)
    0x95, 0x02,                    //     Report Count (2)
    0x81, 0x06,                    //     Input (Data,Var,Rel)

    // Wheel (16-bit = 2 bytes)
    0x09, 0x38,                    //     Usage (Wheel)
    0x16, 0x00, 0x80,              //     Logical Minimum (-32768)
    0x26, 0xFF, 0x7F,              //     Logical Maximum (32767)
    0x75, 0x10,                    //     Report Size (16)
    0x95, 0x01,                    //     Report Count (1)
    0x81, 0x06,                    //     Input (Data,Var,Rel)

    // AC Pan (16-bit = 2 bytes)
    0x05, 0x0C,                    //     Usage Page (Consumer)
    0x0A, 0x38, 0x02,              //     Usage (AC Pan)
    0x16, 0x00, 0x80,              //     Logical Minimum (-32768)
    0x26, 0xFF, 0x7F,              //     Logical Maximum (32767)
    0x75, 0x10,                    //     Report Size (16)
    0x95, 0x01,                    //     Report Count (1)
    0x81, 0x06,                    //     Input (Data,Var,Rel)

    0xC0,                          //   End Collection (Physical)
    0xC0,                          // End Collection (Application)

// --- Keyboard (Report ID 2: 16-byte Bitmap NKRO) ---
    0x05, 0x01,                    // Usage Page (Generic Desktop Ctrls)
    0x09, 0x06,                    // Usage (Keyboard)
    0xA1, 0x01,                    // Collection (Application)
    0x85, 0x02,                    //   Report ID (2)
    0x05, 0x07,                    //   Usage Page (Kbrd/Keypad)
    0x19, 0xE0,                    //   Usage Minimum (0xE0)
    0x29, 0xE7,                    //   Usage Maximum (0xE7)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x01,                    //   Logical Maximum (1)
    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x08,                    //   Report Count (8)
    0x81, 0x02,                    //   Input (Data,Var,Abs)
    0x19, 0x04,                    //   Usage Minimum (0x04)
    0x29, 0x73,                    //   Usage Maximum (0x73)
    0x95, 0x70,                    //   Report Count (112)
    0x81, 0x02,                    //   Input (Data,Var,Abs)
    0x19, 0x87,                    //   Usage Minimum (0x87)
    0x29, 0x8B,                    //   Usage Maximum (0x8B)
    0x95, 0x05,                    //   Report Count (5)
    0x81, 0x02,                    //   Input (Data,Var,Abs)
    0x09, 0x90,                    //   Usage (0x90)
    0x09, 0x91,                    //   Usage (0x91)
    0x95, 0x02,                    //   Report Count (2)
    0x81, 0x02,                    //   Input (Data,Var,Abs)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x03,                    //   Input (Const,Var,Abs)
    0xC0,                          // End Collection
};

static hci_con_handle_t con_handle = HCI_CON_HANDLE_INVALID;
static btstack_packet_callback_registration_t sm_event_callback_registration;

extern "C" void usb_cdc_printf(const char *format, ...);

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event = hci_event_packet_get_type(packet);
    usb_cdc_printf("[BT] Event: 0x%02X\r\n", event);

    switch (event) {
        case BTSTACK_EVENT_STATE: {
            uint8_t state = btstack_event_state_get_state(packet);
            usb_cdc_printf("[BT] BTSTACK_STATE: %d\r\n", state);
            if (state == HCI_STATE_WORKING) {
                ble_led(1);
                gap_advertisements_enable(1);
            }
            break;
        }

        // ホストからの Notification 有効化/無効化やプロトコル変更の確認
        case HCI_EVENT_HIDS_META: {
            uint8_t subevent = hci_event_hids_meta_get_subevent_code(packet);
            usb_cdc_printf("[HIDS] subevent: 0x%02X\r\n", subevent);
            break;
        }

        case HCI_EVENT_LE_META: {
            uint8_t subevent = hci_event_le_meta_get_subevent_code(packet);
            usb_cdc_printf("[BT] LE_META subevent: 0x%02X\r\n", subevent);
            if (subevent == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
                con_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                gap_request_connection_parameter_update(con_handle, 6, 9, 0, 400);
                
                usb_cdc_printf("[BT] Connected! handle=0x%04X\r\n", con_handle);
                ble_led(0);
            }
            break;
        }
        // Security Manager (SM) イベントの処理
        case SM_EVENT_JUST_WORKS_REQUEST: {
            hci_con_handle_t handle = sm_event_just_works_request_get_handle(packet);
            usb_cdc_printf("[BT] SM_EVENT_JUST_WORKS_REQUEST for handle=0x%04X -> Confirming\r\n", handle);
            sm_just_works_confirm(handle);
            break;
        }

        case SM_EVENT_NUMERIC_COMPARISON_REQUEST: {
            hci_con_handle_t handle = sm_event_numeric_comparison_request_get_handle(packet);
            usb_cdc_printf("[BT] SM_EVENT_NUMERIC_COMPARISON_REQUEST for handle=0x%04X -> Confirming\r\n", handle);
            sm_numeric_comparison_confirm(handle);
            break;
        }

        case SM_EVENT_PAIRING_STARTED:
            usb_cdc_printf("[BT] SM_EVENT_PAIRING_STARTED\r\n");
            break;

        case SM_EVENT_PAIRING_COMPLETE: {
            uint8_t status = sm_event_pairing_complete_get_status(packet);
            usb_cdc_printf("[BT] SM_EVENT_PAIRING_COMPLETE status=0x%02X\r\n", status);
            break;
        }

        case HCI_EVENT_ENCRYPTION_CHANGE: {
            uint8_t status = hci_event_encryption_change_get_status(packet);
            uint8_t enabled = hci_event_encryption_change_get_encryption_enabled(packet);
            usb_cdc_printf("[BT] ENCRYPTION_CHANGE status=0x%02X, enabled=%d\r\n", status, enabled);
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            con_handle = HCI_CON_HANDLE_INVALID;
            usb_cdc_printf("[BT] Disconnected!\r\n");
            ble_led(1);
            gap_advertisements_enable(1);
            break;

        default:
            break;
    }
}

void ble_hid_init(void) {
    if (cyw43_arch_init()) {
        return;
    }

    l2cap_init();
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING);

    att_server_init(profile_data, NULL, NULL);
    hids_device_init(0, hid_report_descriptor, sizeof(hid_report_descriptor));

    sm_event_callback_registration.callback = &packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);
    att_server_register_packet_handler(packet_handler);

    static const uint8_t adv_data[] = {
        0x02, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,
        0x0c, BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME, 'P', 'i', 'c', 'o', 'W', ' ', 'C', 'o', 'm', 'b', 'o',
        0x03, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS, 0x12, 0x18,
        0x03, BLUETOOTH_DATA_TYPE_APPEARANCE, 0xc0, 0x03,
    };

    uint16_t adv_int_min = 0x0030;
    uint16_t adv_int_max = 0x0030;
    uint8_t adv_type = 0;
    bd_addr_t null_addr;
    memset(null_addr, 0, 6);
    gap_advertisements_set_params(adv_int_min, adv_int_max, adv_type, 0, null_addr, 0x07, 0x00);
    gap_advertisements_set_data(sizeof(adv_data), (uint8_t*) adv_data);
    gap_advertisements_enable(1);
    hci_power_control(HCI_POWER_ON);

}

void ble_hid_task(void) {
    cyw43_arch_poll();
}

bool ble_hid_is_connected(void) {
    return con_handle != HCI_CON_HANDLE_INVALID;
}

void ble_hid_send_report(uint8_t report_id, const uint8_t* data, uint8_t len) {
    if (con_handle != HCI_CON_HANDLE_INVALID) {
        uint8_t status = hids_device_send_input_report_for_id(con_handle, report_id, (uint8_t*)data, len);
        if (status != 0) {
            usb_cdc_printf("[BLE-ERR] send_report id=%d len=%d status=0x%02X\r\n", report_id, len, status);
        }
    }
}

void ble_pair_new_device(void) {
    if (con_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(con_handle);
    }
    gap_advertisements_enable(1);
}

void ble_clear_bonds(void) {
    int count = le_device_db_count();
    for (int i = count - 1; i >= 0; i--) {
        le_device_db_remove(i);
    }
}

void ble_led(bool value) {
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, value);

}

#endif // ENABLE_BLE