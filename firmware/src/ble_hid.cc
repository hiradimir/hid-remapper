#if ENABLE_BLE

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/unique_id.h"
#include "btstack.h"
#include "btstack_config.h"
#include "ble/le_device_db.h"
#include "ble_mouse.h"
#include "ble_hid.h"
#include "ble/sm.h"

#include "hardware/flash.h"
#include "pico/btstack_flash_bank.h"
#include "btstack_tlv_flash_bank.h"
#include "ble/le_device_db_tlv.h"

#define usb_cdc_printf printf

// Flash 末尾手前の 8KB（セクタ 2 個分）を TLV 領域に割り当て
// hid-remapper (0x1FF000〜) の直前：0x1FD000〜0x1FEFFF
#define TLV_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - (3 * FLASH_SECTOR_SIZE))

// スロット番号を保存するための独自タグ ID ('S' 'L' 'O' 'T')
#define TLV_TAG_CURRENT_SLOT 0x534c4f54

#define MAX_SLOTS NVM_NUM_DEVICE_DB_ENTRIES

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

static bd_addr_t slot_addrs[MAX_SLOTS];
static bool slot_valid[MAX_SLOTS] = {false, false, false, false};
static int current_slot = 0;

static bool is_pairing_mode = false;
static btstack_timer_source_t pairing_timeout_timer;
static btstack_timer_source_t adv_resume_timer;
static hci_con_handle_t con_handle = HCI_CON_HANDLE_INVALID;
static btstack_packet_callback_registration_t sm_event_callback_registration;

static btstack_tlv_flash_bank_t tlv_context;
static const btstack_tlv_t *tlv_impl = NULL;

static volatile bool slot_change_requested = false;
static volatile int pending_slot = -1;
static volatile bool pending_is_pairing = false;

// extern "C" void usb_cdc_printf(const char *format, ...);

// Flash (TLV) から最後に使っていたスロット番号を復元
static void load_current_slot(void) {
    if (!tlv_impl) return;
    uint8_t saved_slot = 0;
    int len = tlv_impl->get_tag(&tlv_context, TLV_TAG_CURRENT_SLOT, &saved_slot, sizeof(saved_slot));
    if (len == sizeof(saved_slot) && saved_slot < MAX_SLOTS) {
        current_slot = saved_slot;
        usb_cdc_printf("[BT] Restored current_slot: %d from TLV\r\n", current_slot + 1);
    } else {
        usb_cdc_printf("[BT] No saved slot found, defaulting to Slot 1\r\n");
    }
}

// Flash (TLV) に現在のスロット番号を保存
static void save_current_slot(uint8_t slot) {
    if (!tlv_impl) return;
    tlv_impl->store_tag(&tlv_context, TLV_TAG_CURRENT_SLOT, &slot, sizeof(slot));
    usb_cdc_printf("[BT] Saved current_slot: %d to TLV\r\n", slot + 1);
}

// タイムアウト時のハンドラ（ペアリングモード終了）
static void pairing_mode_timeout_handler(btstack_timer_source_t *ts) {
    usb_cdc_printf("[BT] Pairing mode timeout.\r\n");
    is_pairing_mode = false;
}

// クールダウン終了後にアドバタイズを再開するハンドラ
static void adv_resume_timer_handler(btstack_timer_source_t *ts) {
    usb_cdc_printf("[BT] Resuming advertisements after cool-down...\r\n");
    ble_led(1);
    gap_advertisements_enable(1);
}

// 基板固有 ID とスロット番号から、BLE規格準拠の Static Random Address を生成
static void get_slot_random_address(int slot, bd_addr_t out_addr) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);

    // Static Random Address の要件: 最上位2ビットが 11 (0xC0以上)
    out_addr[0] = 0xC0 | (id.id[2] & 0x3F);
    out_addr[1] = id.id[3];
    out_addr[2] = id.id[4];
    out_addr[3] = id.id[5];
    out_addr[4] = id.id[6];
    // 末尾バイトにスロット番号をオフセットとして加算（2台目以降も被らない）
    out_addr[5] = id.id[7] + (uint8_t)slot;
}
// スロットごとの名前とアドレスを適用してアドバタイズを安全に再設定する共通関数
static void apply_slot_identity(int slot) {
    current_slot = slot;
    save_current_slot((uint8_t)slot); // Flash に保存

    usb_cdc_printf("[BT] -> apply_slot_identity(Slot %d) start\r\n", slot + 1);

    gap_advertisements_enable(0);

    // デバイス名
    char name_buf[32];
    snprintf(name_buf, sizeof(name_buf), "Remapper [Slot %d]", slot + 1);

    // アドバタイズパケットの構築 (最大 31 バイト)
    uint8_t adv_data[31];
    int adv_data_len = 0;

    // 1. Flags (3 bytes)
    adv_data[adv_data_len++] = 2;
    adv_data[adv_data_len++] = BLUETOOTH_DATA_TYPE_FLAGS;
    adv_data[adv_data_len++] = 0x06;

    // 2. 16-bit Service UUID: HID Service 0x1812 (4 bytes) - これがないとMacに表示されない
    adv_data[adv_data_len++] = 3;
    adv_data[adv_data_len++] = BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS;
    adv_data[adv_data_len++] = 0x12;
    adv_data[adv_data_len++] = 0x18;

    // 3. Appearance: Generic HID 0x03C0 (4 bytes)
    adv_data[adv_data_len++] = 3;
    adv_data[adv_data_len++] = BLUETOOTH_DATA_TYPE_APPEARANCE;
    adv_data[adv_data_len++] = 0xC0;
    adv_data[adv_data_len++] = 0x03;

    // 4. Complete Local Name (最大 19 bytes)
    int name_len = strlen(name_buf);
    if (name_len > 18) name_len = 18; // 31バイト上限ガード
    adv_data[adv_data_len++] = name_len + 1;
    adv_data[adv_data_len++] = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
    memcpy(&adv_data[adv_data_len], name_buf, name_len);
    adv_data_len += name_len;

    gap_advertisements_set_data(adv_data_len, adv_data);

    // スロット固有の Static Random Address を適用
    bd_addr_t slot_addr;
    get_slot_random_address(slot, slot_addr);
    gap_random_address_set_mode(GAP_RANDOM_ADDRESS_TYPE_STATIC);
    gap_random_address_set(slot_addr);

    // アドバタイズ再開
    ble_led(1);
    gap_advertisements_enable(1);
    usb_cdc_printf("[BT] <- apply_slot_identity done. ADDR=%s\r\n", bd_addr_to_str(slot_addr));
}

// ホットキーから呼ばれる関数（ここではフラグを立てるだけで即座に抜ける）
void ble_switch_slot(int slot) {
    if (slot < 0 || slot >= MAX_SLOTS) return;
    usb_cdc_printf("[BT] Request Switch to Slot %d\r\n", slot + 1);
    pending_slot = slot;
    pending_is_pairing = false;
    slot_change_requested = true;
}

void ble_enter_pairing_mode(int slot) {
    if (slot < 0 || slot >= MAX_SLOTS) return;
    usb_cdc_printf("[BT] Request Pairing Mode for Slot %d\r\n", slot + 1);
    pending_slot = slot;
    pending_is_pairing = true;
    slot_change_requested = true;
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event = hci_event_packet_get_type(packet);
    usb_cdc_printf("[BT] Event: 0x%02X\r\n", event);

    switch (event) {
        case BTSTACK_EVENT_STATE: {
            uint8_t state = btstack_event_state_get_state(packet);
            usb_cdc_printf("[BT] BTSTACK_STATE: %d\r\n", state);
            if (state == HCI_STATE_WORKING) {
                // チップが完全に起動したこの瞬間に初期スロットを適用する
                apply_slot_identity(current_slot);
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
            if (subevent == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
                con_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                bd_addr_t peer_addr;
                hci_subevent_le_connection_complete_get_peer_address(packet, peer_addr);

                is_pairing_mode = false;
                slot_valid[current_slot] = true;

                // 通信パラメータの最適化要求
                gap_request_connection_parameter_update(con_handle, 6, 9, 0, 400);
                usb_cdc_printf("[BT] Connected successfully to Slot %d! peer=%s\r\n", current_slot + 1, bd_addr_to_str(peer_addr));
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
            if (status == ERROR_CODE_SUCCESS) {
                // 新規ペアリングが成功したらペアリングモードを終了
                is_pairing_mode = false;
                btstack_run_loop_remove_timer(&pairing_timeout_timer);
            }
            break;
        }

        case HCI_EVENT_ENCRYPTION_CHANGE: {
            uint8_t status = hci_event_encryption_change_get_status(packet);
            uint8_t enabled = hci_event_encryption_change_get_encryption_enabled(packet);
            usb_cdc_printf("[BT] ENCRYPTION_CHANGE status=0x%02X, enabled=%d\r\n", status, enabled);
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            con_handle = HCI_CON_HANDLE_INVALID;
            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            usb_cdc_printf("[BT] Disconnected! reason=0x%02X\r\n", reason);
            ble_led(1);
            gap_advertisements_enable(1);
            break;
        }

        default:
            break;
    }
}

void ble_hid_init(void) {
    if (cyw43_arch_init()) {
        return;
    }

    // 1. Flash Bank インスタンスを取得（PICO_FLASH_BANK_STORAGE_OFFSET が自動適用）
    const hal_flash_bank_t *hal_flash_bank_impl = pico_flash_bank_instance();

    // 2. BTstack TLV Flash Bank の初期化
    tlv_impl = btstack_tlv_flash_bank_init_instance(
        &tlv_context,
        hal_flash_bank_impl,
        NULL
    );
    btstack_tlv_set_instance(tlv_impl, &tlv_context);

    // 3. スロット番号の復元
    load_current_slot();

    l2cap_init();

    // 4. ペアリング鍵 DB に TLV を接続
    le_device_db_tlv_configure(tlv_impl, &tlv_context);

    // 5. SM の初期化
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING);

    att_server_init(profile_data, NULL, NULL);
    hids_device_init(0, hid_report_descriptor, sizeof(hid_report_descriptor));

    sm_event_callback_registration.callback = &packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);
    att_server_register_packet_handler(packet_handler);

    uint16_t adv_int_min = 0x0030;
    uint16_t adv_int_max = 0x0030;
    bd_addr_t null_addr;
    memset(null_addr, 0, 6);
    gap_advertisements_set_params(adv_int_min, adv_int_max, 0, 0, null_addr, 0x07, 0x00);

    hci_power_control(HCI_POWER_ON);
}


void ble_hid_task(void) {
    if (slot_change_requested) {
        slot_change_requested = false;
        int slot = pending_slot;
        bool is_pairing = pending_is_pairing;

        usb_cdc_printf("[BT] Processing slot change to %d (pairing=%d)\r\n", slot + 1, is_pairing);

        is_pairing_mode = is_pairing;
        if (is_pairing) {
            btstack_run_loop_remove_timer(&pairing_timeout_timer);
            btstack_run_loop_set_timer_handler(&pairing_timeout_timer, pairing_mode_timeout_handler);
            btstack_run_loop_set_timer(&pairing_timeout_timer, 60000);
            btstack_run_loop_add_timer(&pairing_timeout_timer);
        }

        // 既存接続があれば切断
        if (con_handle != HCI_CON_HANDLE_INVALID) {
            gap_disconnect(con_handle);
        }

        // 新しいスロットのアイデンティティを適用
        apply_slot_identity(slot);
    }
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