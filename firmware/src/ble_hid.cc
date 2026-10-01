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
#include "ble/gatt-service/battery_service_server.h"
#include "ble/sm.h"

#include "hardware/flash.h"
#include "pico/btstack_flash_bank.h"
#include "btstack_tlv_flash_bank.h"
#include "ble/le_device_db_tlv.h"
#include "oled.h"

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

static hids_device_report_t hid_reports[2];
static bool ble_reports_enabled = false;
static bool ble_can_send = true;
static bool slot_disconnect_after_zero = false;
static bool zero_sent_waiting_ack = false;
static bd_addr_t slot_addrs[MAX_SLOTS];
static bool slot_valid[MAX_SLOTS] = {false, false, false, false};
static int current_slot = 0;

static bool is_pairing_mode = false;
static btstack_timer_source_t pairing_timeout_timer;
static btstack_timer_source_t adv_resume_timer;
static btstack_timer_source_t disconnect_timeout_timer;
static hci_con_handle_t con_handle = HCI_CON_HANDLE_INVALID;
static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;

static btstack_tlv_flash_bank_t tlv_context;
static const btstack_tlv_t *tlv_impl = NULL;

static volatile bool slot_change_requested = false;
static volatile int pending_slot = -1;
static volatile bool pending_is_pairing = false;
static volatile bool switch_pending = false;

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
        usb_cdc_printf("[BT] No saved slot found (len=%d), defaulting to Slot 1\r\n", len);
    }
    int num_bonded = le_device_db_count();
    usb_cdc_printf("[BT] Bonded devices in DB: %d\r\n", num_bonded);
    oled_set_slot(current_slot);
    oled_set_bt_state(OLED_BT_DISCONNECTED);
    for (int i = 0; i < 4; i++) {
        oled_set_slot_bonded(i, i < num_bonded);
    }
}

// Flash (TLV) に現在のスロット番号を保存
static void save_current_slot(uint8_t slot) {
    if (!tlv_impl) return;
    uint8_t current_saved = 0xFF;
    int len = tlv_impl->get_tag(&tlv_context, TLV_TAG_CURRENT_SLOT, &current_saved, sizeof(current_saved));
    if (len == sizeof(current_saved) && current_saved == slot) {
        return;
    }
    int res = tlv_impl->store_tag(&tlv_context, TLV_TAG_CURRENT_SLOT, &slot, sizeof(slot));
    usb_cdc_printf("[BT] Saved current_slot: %d to TLV (res=%d)\r\n", slot + 1, res);
}

// タイムアウト時のハンドラ（ペアリングモード終了）
static void pairing_mode_timeout_handler(btstack_timer_source_t *ts) {
    usb_cdc_printf("[BT] Pairing mode timeout.\r\n");
    is_pairing_mode = false;
    bd_addr_t null_addr = {0};
    gap_advertisements_set_params(0x00A0, 0x00C0, 0, 0, null_addr, 0x07, 0x00);
    oled_set_bt_state(OLED_BT_DISCONNECTED);
    oled_set_status("Timeout");
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
// ホストが休止・サスペンド等で切断シーケンスに応答しない場合の安全タイムアウト (300ms)
static void disconnect_timeout_handler(btstack_timer_source_t *ts) {
    if (con_handle != HCI_CON_HANDLE_INVALID && slot_disconnect_after_zero) {
        usb_cdc_printf("[BT] Disconnect timeout (host suspended/unresponsive). Forcing disconnect.\r\n");
        slot_disconnect_after_zero = false;
        zero_sent_waiting_ack = false;
        switch_pending = true;
        gap_disconnect(con_handle);
    }
}

// スロットごとの名前とアドレスを適用してアドバタイズを安全に再設定する共通関数
static void apply_slot_identity(int slot) {
    current_slot = slot;
    save_current_slot((uint8_t)slot); // Flash に保存

    usb_cdc_printf("[BT] -> apply_slot_identity(Slot %d) start\r\n", slot + 1);
    oled_set_slot(slot);
    oled_set_bt_state(is_pairing_mode ? OLED_BT_PAIRING : OLED_BT_DISCONNECTED);

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
    oled_set_slot(slot);
    oled_set_bt_state(OLED_BT_SWITCHING);
    oled_set_peer_addr("");
    oled_set_status("Switching...");
}

void ble_hid_switch_host(uint8_t slot) {
    ble_switch_slot((int)slot);
}

void ble_enter_pairing_mode(int slot) {
    if (slot < 0 || slot >= MAX_SLOTS) return;
    usb_cdc_printf("[BT] Request Pairing Mode for Slot %d\r\n", slot + 1);
    pending_slot = slot;
    pending_is_pairing = true;
    slot_change_requested = true;
    oled_set_slot(slot);
    oled_set_bt_state(OLED_BT_PAIRING);
    oled_set_peer_addr("");
    oled_set_status("Pairing (60s)");
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event = hci_event_packet_get_type(packet);

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
            switch (subevent) {
                case HIDS_SUBEVENT_INPUT_REPORT_ENABLE: {
                    con_handle = hids_subevent_input_report_enable_get_con_handle(packet);
                    bool enabled = hids_subevent_input_report_enable_get_enable(packet) != 0;
                    uint8_t report_id = hids_subevent_input_report_enable_get_report_id(packet);
                    if (enabled) {
                        usb_cdc_printf("[BT] Input report enabled for ID %d (Ready)\r\n", report_id);
                        ble_reports_enabled = true;
                        ble_can_send = true;
                        ble_led(0);
                    }
                    break;
                }
                case HIDS_SUBEVENT_CAN_SEND_NOW: {
                    ble_can_send = true;
                    if (slot_disconnect_after_zero) {
                        if (!zero_sent_waiting_ack) {
                            // 送信バッファが空いたので、ホストへ全キー離下レポート（zero_kbd）を確実に送信
                            uint8_t zero_kbd[16] = {0};
                            uint8_t status = hids_device_send_input_report_for_id(con_handle, 2, zero_kbd, sizeof(zero_kbd));
                            if (status == ERROR_CODE_SUCCESS) {
                                ble_can_send = false;
                                zero_sent_waiting_ack = true;
                                hids_device_request_can_send_now_event(con_handle);
                                break;
                            }
                        } else {
                            // zero_kbd の無線送信完了が確認できた！これでホスト側のキーは完全に解除されたので安全に切断
                            slot_disconnect_after_zero = false;
                            zero_sent_waiting_ack = false;
                            switch_pending = true;
                            gap_disconnect(con_handle);
                            break;
                        }
                    }
                    break;
                }
                default:
                    break;
            }
            break;
        }

        case HCI_EVENT_LE_META: {
            uint8_t subevent = hci_event_le_meta_get_subevent_code(packet);
            if (subevent == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
                hci_con_handle_t new_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                if (con_handle == HCI_CON_HANDLE_INVALID) {
                    con_handle = new_handle;
                    bd_addr_t peer_addr;
                    hci_subevent_le_connection_complete_get_peer_address(packet, peer_addr);

                    is_pairing_mode = false;
                    slot_valid[current_slot] = true;

                    ble_reports_enabled = false;
                    ble_can_send = true;
                    slot_disconnect_after_zero = false;
                    zero_sent_waiting_ack = false;
                    gap_advertisements_enable(0);
                    usb_cdc_printf("[BT] Connected to Slot %d (peer=%s)\r\n", current_slot + 1, bd_addr_to_str(peer_addr));
                    oled_set_slot(current_slot);
                    oled_set_bt_state(OLED_BT_CONNECTED);
                    oled_set_slot_bonded(current_slot, true);
                    oled_set_peer_addr(bd_addr_to_str(peer_addr));
                }
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
            oled_set_bt_state(OLED_BT_PAIRING);
            oled_set_status("Pairing...");
            break;

        case SM_EVENT_PAIRING_COMPLETE: {
            uint8_t status = sm_event_pairing_complete_get_status(packet);
            usb_cdc_printf("[BT] SM_EVENT_PAIRING_COMPLETE status=0x%02X\r\n", status);
            if (status == ERROR_CODE_SUCCESS) {
                // 新規ペアリングが成功したらペアリングモードを終了
                is_pairing_mode = false;
                btstack_run_loop_remove_timer(&pairing_timeout_timer);
                int count = le_device_db_count();
                usb_cdc_printf("[BT] Pairing complete, total bonded devices: %d\r\n", count);
                bd_addr_t null_addr = {0};
                gap_advertisements_set_params(0x00A0, 0x00C0, 0, 0, null_addr, 0x07, 0x00);
                oled_set_slot(current_slot);
                oled_set_bt_state(OLED_BT_CONNECTED);
                oled_set_slot_bonded(current_slot, true);
            }
            break;
        }

        // 既知ホストの再接続検知
        case SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED: {
            hci_con_handle_t handle = sm_event_identity_resolving_succeeded_get_handle(packet);
            if (is_pairing_mode) {
                // ペアリングモード中に既知ホストが再接続してきた場合は即座に切断し、オープン広告を維持
                usb_cdc_printf("[BT] Known host reconnected during pairing mode! Disconnecting handle 0x%04X\r\n", handle);
                gap_disconnect(handle);
                gap_advertisements_enable(1);
                break;
            }
            int device_index = sm_event_identity_resolving_succeeded_get_index(packet);
            usb_cdc_printf("[BT] Identity resolved: device_index=%d\r\n", device_index);
            break;
        }

        case HCI_EVENT_ENCRYPTION_CHANGE: {
            uint8_t status = hci_event_encryption_change_get_status(packet);
            uint8_t enabled = hci_event_encryption_change_get_encryption_enabled(packet);
            if (status == 0 && enabled) {
                usb_cdc_printf("[BT] Encryption enabled\r\n");
                ble_reports_enabled = true;
                ble_can_send = true;
                ble_led(0);
            } else if (status != 0) {
                usb_cdc_printf("[BT] Encryption failed (status=0x%02X)\r\n", status);
            }
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            btstack_run_loop_remove_timer(&disconnect_timeout_timer);
            if (con_handle != HCI_CON_HANDLE_INVALID) {
                con_handle = HCI_CON_HANDLE_INVALID;
                ble_reports_enabled = false;
                ble_can_send = true;
                slot_disconnect_after_zero = false;
                zero_sent_waiting_ack = false;
                uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
                usb_cdc_printf("[BT] Disconnected (reason=0x%02X)\r\n", reason);
                oled_set_slot(current_slot);
                oled_set_bt_state(OLED_BT_DISCONNECTED);
                oled_set_peer_addr("");
                oled_set_status("Disconnected");

            // スロット切り替え待ちがある場合、切断完了後に安全に新スロットを適用して再開
            if (switch_pending) {
                switch_pending = false;
                int slot = pending_slot;
                is_pairing_mode = pending_is_pairing;
                if (is_pairing_mode) {
                    btstack_run_loop_remove_timer(&pairing_timeout_timer);
                    btstack_run_loop_set_timer_handler(&pairing_timeout_timer, pairing_mode_timeout_handler);
                    btstack_run_loop_set_timer(&pairing_timeout_timer, 60000);
                    btstack_run_loop_add_timer(&pairing_timeout_timer);
                    bd_addr_t null_addr = {0};
                    gap_advertisements_set_params(0x0050, 0x0060, 0, 0, null_addr, 0x07, 0x00);
                } else {
                    bd_addr_t null_addr = {0};
                    gap_advertisements_set_params(0x00A0, 0x00C0, 0, 0, null_addr, 0x07, 0x00);
                }
                usb_cdc_printf("[BT] Applying new slot identity after disconnect: Slot %d (pairing=%d)\r\n", slot + 1, is_pairing_mode);
                apply_slot_identity(slot);
            } else {
                ble_led(1);
                gap_advertisements_enable(1);
            }
            }
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
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);

    att_server_init(profile_data, NULL, NULL);
    battery_service_server_init(100);
    hids_device_init_with_storage(0, hid_report_descriptor, sizeof(hid_report_descriptor), 2, hid_reports);
    hids_device_register_packet_handler(packet_handler);

    sm_event_callback_registration.callback = &packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);
    att_server_register_packet_handler(packet_handler);

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // アドバタイズ間隔を 100ms〜120ms (0x00A0〜0x00C0) に設定
    uint16_t adv_int_min = 0x00A0; // 100 ms
    uint16_t adv_int_max = 0x00C0; // 120 ms
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

        // 既存接続がある場合は、必ずホストへ全キー離下（zero_kbd）を届けて完了を確認してから切断する
        if (con_handle != HCI_CON_HANDLE_INVALID) {
            slot_disconnect_after_zero = true;
            zero_sent_waiting_ack = false;
            switch_pending = true;

            // ホストが休止・サスペンド等で応答しない場合の安全タイムアウト (300ms)
            btstack_run_loop_remove_timer(&disconnect_timeout_timer);
            btstack_run_loop_set_timer_handler(&disconnect_timeout_timer, disconnect_timeout_handler);
            btstack_run_loop_set_timer(&disconnect_timeout_timer, 300);
            btstack_run_loop_add_timer(&disconnect_timeout_timer);
            if (ble_can_send) {
                uint8_t zero_kbd[16] = {0};
                uint8_t status = hids_device_send_input_report_for_id(con_handle, 2, zero_kbd, sizeof(zero_kbd));
                if (status == ERROR_CODE_SUCCESS) {
                    ble_can_send = false;
                    zero_sent_waiting_ack = true;
                    hids_device_request_can_send_now_event(con_handle);
                } else {
                    hids_device_request_can_send_now_event(con_handle);
                }
            } else {
                hids_device_request_can_send_now_event(con_handle);
            }
        } else {
            // 未接続状態であれば即座に新スロットを適用
            switch_pending = false;
            is_pairing_mode = is_pairing;
            if (is_pairing) {
                btstack_run_loop_remove_timer(&pairing_timeout_timer);
                btstack_run_loop_set_timer_handler(&pairing_timeout_timer, pairing_mode_timeout_handler);
                btstack_run_loop_set_timer(&pairing_timeout_timer, 60000);
                btstack_run_loop_add_timer(&pairing_timeout_timer);
            }
            apply_slot_identity(slot);
        }
    }
}

bool ble_hid_is_connected(void) {
    return con_handle != HCI_CON_HANDLE_INVALID;
}

bool ble_hid_can_send(void) {
    return (con_handle != HCI_CON_HANDLE_INVALID) && ble_reports_enabled && ble_can_send && !slot_disconnect_after_zero;
}

void ble_hid_send_report(uint8_t report_id, const uint8_t* data, uint8_t len) {
    if (con_handle != HCI_CON_HANDLE_INVALID && ble_reports_enabled) {
        oled_notify_tx();
        uint8_t status = hids_device_send_input_report_for_id(con_handle, report_id, (uint8_t*)data, len);
        if (status == ERROR_CODE_SUCCESS) {
            ble_can_send = false;
            if (hids_device_request_can_send_now_event(con_handle) != ERROR_CODE_SUCCESS) {
                ble_can_send = true;
            }
        } else {
            usb_cdc_printf("[BLE-ERR] send_report id=%d len=%d status=0x%02X\r\n", report_id, len, status);
            ble_can_send = false;
            if (hids_device_request_can_send_now_event(con_handle) != ERROR_CODE_SUCCESS) {
                ble_can_send = true;
            }
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