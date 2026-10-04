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


// Flash 末尾手前の 8KB（セクタ 2 個分）を TLV 領域に割り当て
// hid-remapper (0x1FF000〜) の直前：0x1FD000〜0x1FEFFF
#define TLV_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - (3 * FLASH_SECTOR_SIZE))

// スロット番号を保存するための独自タグ ID ('S' 'L' 'O' 'T')
#define TLV_TAG_CURRENT_SLOT 0x534c4f54
// ボンド済みスロットのビットマスクを保存する独自タグ ID ('S' 'B' 'N' 'D')
#define TLV_TAG_SLOT_BOND_MASK 0x53424E32
// スロットごとの登録ピアアドレス情報を保存する独自タグ ID ('S' 'P' 'E' '2')
#define TLV_TAG_SLOT_PEERS 0x53504532
// スロットごとのMACアドレスソルト（世代番号）を保存する独自タグ ID ('S' 'A' 'L' 'T')
#define TLV_TAG_SLOT_SALTS 0x53414C54

#define MAX_SLOTS NVM_NUM_DEVICE_DB_ENTRIES

typedef struct {
    uint8_t valid;
    uint8_t addr_type;
    bd_addr_t addr;
} slot_peer_t;

static uint8_t slot_bonded_mask = 0;
static slot_peer_t slot_peers[MAX_SLOTS];
static uint8_t slot_salts[MAX_SLOTS];


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

// 【BL-14】統一ステートマシンの定義
typedef enum {
    BLE_STATE_STANDBY_SILENT = 0, // 未ボンドスロット：電波完全停止 (OLED: DISC, LED: OFF, Status: Ready)
    BLE_STATE_PAIRING = 1,        // ペアリング待機中 (60s限定・高速30ms・Flags:0x06, LED: ON, OLED: PAIR, Status: Pairing (60s))
    BLE_STATE_RECONNECTING = 2,   // ボンド済み再接続待機中 (30ms・Flags:0x04, LED: OFF, OLED: DISC, Status: Ready)
    BLE_STATE_CONNECTING = 3,     // 物理リンク接続中（暗号化・購読未完了、送信完全ブロック, OLED: WAIT, Status: Connecting...）
    BLE_STATE_CONNECTED = 4,      // 暗号化・レポート購読完了（論理接続確立、送信許可, OLED: CONN, Status: Conn: <addr>）
    BLE_STATE_SWITCHING = 5,      // スロット切替遷移中（切断待ち, OLED: WAIT, Status: Switching...）
} ble_state_t;

static ble_state_t current_state = BLE_STATE_STANDBY_SILENT;

// 【BL-15】暗号化および各レポート有効化の厳密な個別管理
static bool ble_kbd_ready = false;      // Report ID 2 (Keyboard) subscribed
static bool ble_mouse_ready = false;    // Report ID 1 (Mouse) subscribed
static bool ble_encrypted = false;      // Link encrypted
static bool ble_can_send = true;
static bool slot_disconnect_after_zero = false;
static bool zero_sent_waiting_ack = false;
static char current_peer_addr[18] = "";

static uint16_t ble_conn_interval = 0;

uint16_t ble_hid_get_conn_interval(void) {
    return ble_conn_interval;
}


static int current_slot = 0;

static bool is_pairing_mode = false;
static btstack_timer_source_t pairing_timeout_timer;
static btstack_timer_source_t disconnect_timeout_timer;
static btstack_timer_source_t conn_stabilize_timer;
static btstack_timer_source_t can_send_timeout_timer;
static btstack_timer_source_t reconnect_timeout_timer;
static btstack_timer_source_t conn_handshake_timeout_timer;
static bool adv_cooldown_active = false;
static hci_con_handle_t con_handle = HCI_CON_HANDLE_INVALID;
static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;

static btstack_tlv_flash_bank_t tlv_context;
static const btstack_tlv_t *tlv_impl = NULL;

static volatile bool slot_change_requested = false;
static volatile int pending_slot = -1;
static volatile bool pending_is_pairing = false;
static volatile bool pending_toggle_off = false;
static volatile bool switch_pending = false;

typedef enum {
    ADV_STEP_IDLE = 0,
    ADV_STEP_SET_ADDR,
    ADV_STEP_SET_DATA,
    ADV_STEP_ENABLE
} adv_step_t;

static adv_step_t adv_step = ADV_STEP_IDLE;
static btstack_timer_source_t adv_step_timer;
static void adv_step_timer_handler(btstack_timer_source_t *ts);

static void apply_slot_identity(int slot);
static void update_ui_for_state(ble_state_t state);
static void save_slot_bonded_mask(uint8_t mask);
static void save_slot_peers(void);

// 状態に応じた OLED / LED の一元更新
static void update_ui_for_state(ble_state_t state) {
    oled_set_slot((uint8_t)current_slot);
    switch (state) {
        case BLE_STATE_STANDBY_SILENT:
            oled_set_bt_state(OLED_BT_DISCONNECTED);
            oled_set_mode(false);
            oled_set_status("Ready");
            oled_set_peer_addr("");
            ble_led(0);
            break;
        case BLE_STATE_PAIRING:
            oled_set_bt_state(OLED_BT_PAIRING);
            oled_set_mode(false);
            oled_set_status("Pairing (60s)");
            oled_set_peer_addr("");
            ble_led(1);
            break;
        case BLE_STATE_RECONNECTING:
            oled_set_bt_state(OLED_BT_SWITCHING); // WAIT アイコン（接続試行中）
            oled_set_mode(false);
            oled_set_status("Connecting...");     // ボンド情報あり：接続試行中
            oled_set_peer_addr("");
            ble_led(0);
            break;
        case BLE_STATE_CONNECTING:
            oled_set_bt_state(OLED_BT_SWITCHING); // WAIT
            oled_set_mode(false);
            oled_set_status("Connecting...");
            oled_set_peer_addr("");
            ble_led(0);
            break;
        case BLE_STATE_CONNECTED:
            oled_set_bt_state(OLED_BT_CONNECTED);
            oled_set_mode(true);
            ble_led(0);
            if (current_peer_addr[0] != '\0') {
                oled_set_peer_addr(current_peer_addr);
            }
            break;
        case BLE_STATE_SWITCHING:
            oled_set_bt_state(OLED_BT_SWITCHING); // WAIT
            oled_set_mode(false);
            oled_set_status("Switching...");
            oled_set_peer_addr("");
            ble_led(0);
            break;
    }
}

static void can_send_timeout_handler(btstack_timer_source_t *ts) {
    if (!ble_can_send && con_handle != HCI_CON_HANDLE_INVALID) {
        printf("[BT] WARNING: CAN_SEND_NOW watchdog timed out (2000ms). Resetting flow control.\r\n");
        ble_can_send = true;
        hids_device_request_can_send_now_event(con_handle);
    }
}

static void reconnect_timeout_handler(btstack_timer_source_t *ts) {
    if (con_handle == HCI_CON_HANDLE_INVALID && current_state == BLE_STATE_RECONNECTING) {
        printf("[BT] Reconnection timeout (30s expired). Radio OFF.\r\n");
        gap_advertisements_enable(0);
        current_state = BLE_STATE_STANDBY_SILENT;
        update_ui_for_state(current_state);
        oled_set_status("Ready");
    }
}

static void conn_handshake_timeout_handler(btstack_timer_source_t *ts) {
    if (con_handle != HCI_CON_HANDLE_INVALID && current_state == BLE_STATE_CONNECTING) {
        printf("[BT] Handshake timeout (10s expired without encryption/subscription). Disconnecting stalled link.\r\n");
        gap_disconnect(con_handle);
    }
}

static void conn_stabilize_timer_handler(btstack_timer_source_t *ts) {
    if (con_handle != HCI_CON_HANDLE_INVALID && ble_kbd_ready && ble_encrypted) {
        printf("[BT] Connection stabilized -> CONNECTED (Ready to send)\r\n");
        btstack_run_loop_remove_timer(&conn_handshake_timeout_timer);
        current_state = BLE_STATE_CONNECTED;
        update_ui_for_state(current_state);

        // 【極めて重要】接続安定化・暗号化完了直後に低遅延パラメータ（7.5ms〜11.25ms）を要求
        // これがないとホスト（macOS/Win/Android）は 30ms〜50ms の省電力低速インターバルで動作し、
        // 1秒間に20〜30パケットしか通らなくなるため、マウスが激しく飛び飛び（カクつき・遅延累積）になる。
        gap_request_connection_parameter_update(con_handle, 9, 12, 0, 400);

        // ハンドシェイク完了・通信安定後に安全に Flash へ永続化（SPI 通信中の Flash 消去ブロック回避）
        save_slot_bonded_mask(slot_bonded_mask);
        save_slot_peers();
    }
}

static void check_and_start_connection_stabilize(void) {
    if (ble_kbd_ready && ble_encrypted && con_handle != HCI_CON_HANDLE_INVALID) {
        printf("[BT] Handshake complete (Encrypted + KBD Ready). Stabilizing for 100ms...\r\n");
        btstack_run_loop_remove_timer(&conn_stabilize_timer);
        btstack_run_loop_set_timer_handler(&conn_stabilize_timer, conn_stabilize_timer_handler);
        btstack_run_loop_set_timer(&conn_stabilize_timer, 100);
        btstack_run_loop_add_timer(&conn_stabilize_timer);
    }
}

static void save_slot_bonded_mask(uint8_t mask) {
    slot_bonded_mask = mask;
    if (!tlv_impl) return;
    tlv_impl->store_tag(&tlv_context, TLV_TAG_SLOT_BOND_MASK, &mask, sizeof(mask));
}

static void save_slot_peers(void) {
    if (!tlv_impl) return;
    tlv_impl->store_tag(&tlv_context, TLV_TAG_SLOT_PEERS, (uint8_t*)slot_peers, sizeof(slot_peers));
}

static void save_slot_salts(void) {
    if (!tlv_impl) return;
    tlv_impl->store_tag(&tlv_context, TLV_TAG_SLOT_SALTS, (uint8_t*)slot_salts, sizeof(slot_salts));
}

static void load_slot_salts(void) {
    if (!tlv_impl) return;
    int len = tlv_impl->get_tag(&tlv_context, TLV_TAG_SLOT_SALTS, (uint8_t*)slot_salts, sizeof(slot_salts));
    if (len != sizeof(slot_salts)) {
        memset(slot_salts, 0, sizeof(slot_salts));
    }
}

static void load_slot_peers(void) {
    if (!tlv_impl) return;
    int len = tlv_impl->get_tag(&tlv_context, TLV_TAG_SLOT_PEERS, (uint8_t*)slot_peers, sizeof(slot_peers));
    if (len != sizeof(slot_peers)) {
        memset(slot_peers, 0, sizeof(slot_peers));
    }
}

// Flash (TLV) から最後に使っていたスロット番号とボンド情報を復元

static bool is_slot_bonded(int slot) {
    if (slot < 0 || slot >= MAX_SLOTS) return false;
    if (is_pairing_mode && slot == current_slot) return false;
    return (slot_bonded_mask & (1 << slot)) != 0;
}

static void load_current_slot(void) {
    if (!tlv_impl) return;
    uint8_t saved_slot = 0;
    int len = tlv_impl->get_tag(&tlv_context, TLV_TAG_CURRENT_SLOT, &saved_slot, sizeof(saved_slot));
    if (len == sizeof(saved_slot) && saved_slot < MAX_SLOTS) {
        current_slot = saved_slot;
        printf("[BT] Restored current_slot: %d from TLV\r\n", current_slot + 1);
    } else {
        printf("[BT] No saved slot found (len=%d), defaulting to Slot 1\r\n", len);
    }

    uint8_t saved_mask = 0;
    len = tlv_impl->get_tag(&tlv_context, TLV_TAG_SLOT_BOND_MASK, &saved_mask, sizeof(saved_mask));
    if (len == sizeof(saved_mask)) {
        slot_bonded_mask = saved_mask;
    } else {
        slot_bonded_mask = 0;
    }

    load_slot_peers();
    load_slot_salts();

    int num_bonded = le_device_db_count();

    // 1) slot_peers に記録があり、かつ DB に該当アドレスが存在するか確認
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (slot_peers[i].valid) {
            bool found = false;
            for (int j = 0; j < num_bonded; j++) {
                int a_type = BD_ADDR_TYPE_UNKNOWN;
                bd_addr_t a_addr;
                le_device_db_info(j, &a_type, a_addr, NULL);
                if (memcmp(a_addr, slot_peers[i].addr, 6) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                // DB から削除されていた場合は無効化
                slot_peers[i].valid = 0;
                memset(slot_peers[i].addr, 0, 6);
                slot_bonded_mask &= ~(1 << i);
            }
        }
    }

    // 2) もし DB にボンドが存在するが slot_peers に未登録の場合（旧ファームウェアからの移行）
    // DB 内のデバイスを未登録スロットへ自動マッピングしてペアリング情報を救出
    if (num_bonded > 0) {
        for (int j = 0; j < num_bonded && j < MAX_SLOTS; j++) {
            int a_type = BD_ADDR_TYPE_UNKNOWN;
            bd_addr_t a_addr;
            le_device_db_info(j, &a_type, a_addr, NULL);
            if (a_type == BD_ADDR_TYPE_UNKNOWN) continue;

            // このデバイスが既にいずれかのスロットに登録されているか確認
            bool already_mapped = false;
            for (int i = 0; i < MAX_SLOTS; i++) {
                if (slot_peers[i].valid && memcmp(slot_peers[i].addr, a_addr, 6) == 0) {
                    already_mapped = true;
                    break;
                }
            }
            if (!already_mapped) {
                // 空いているスロット（j番目または最初の空きスロット）に自動割り当て
                int target_slot = -1;
                if (!slot_peers[j].valid) {
                    target_slot = j;
                } else {
                    for (int i = 0; i < MAX_SLOTS; i++) {
                        if (!slot_peers[i].valid) { target_slot = i; break; }
                    }
                }
                if (target_slot >= 0) {
                    slot_peers[target_slot].valid = 1;
                    slot_peers[target_slot].addr_type = a_type;
                    memcpy(slot_peers[target_slot].addr, a_addr, 6);
                    slot_bonded_mask |= (1 << target_slot);
                    printf("[BT] Migrated DB entry %d (%s) to Slot %d\r\n", j, bd_addr_to_str(a_addr), target_slot + 1);
                }
            }
        }
    }

    // 整合性更新結果を保存
    save_slot_peers();
    save_slot_bonded_mask(slot_bonded_mask);

    printf("[BT] Bonded devices in DB: %d, slot_bonded_mask: 0x%02X\r\n", num_bonded, slot_bonded_mask);
    oled_set_slot(current_slot);
    oled_set_bt_state(OLED_BT_DISCONNECTED);
    for (int i = 0; i < 4; i++) {
        oled_set_slot_bonded(i, (slot_bonded_mask & (1 << i)) != 0);
    }
}
static void save_current_slot(uint8_t slot) {
    if (!tlv_impl) return;
    uint8_t current_saved = 0xFF;
    int len = tlv_impl->get_tag(&tlv_context, TLV_TAG_CURRENT_SLOT, &current_saved, sizeof(current_saved));
    if (len == sizeof(current_saved) && current_saved == slot) {
        return;
    }
    int res = tlv_impl->store_tag(&tlv_context, TLV_TAG_CURRENT_SLOT, &slot, sizeof(slot));
    printf("[BT] Saved current_slot: %d to TLV (res=%d)\r\n", slot + 1, res);
}

// タイムアウト時のハンドラ（ペアリングモード終了）
static void pairing_mode_timeout_handler(btstack_timer_source_t *ts) {
    printf("[BT] Pairing mode timeout (60s expired).\r\n");
    is_pairing_mode = false;
    oled_set_bt_state(OLED_BT_DISCONNECTED);
    oled_set_status("Timeout");

    // ペアリングモード終了：未ボンドなら電波停止、ボンド済みなら非公開再接続モードへ戻す
    apply_slot_identity(current_slot);
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
    out_addr[4] = id.id[6] ^ slot_salts[slot];
    // 末尾バイトにスロット番号およびソルトを加算（ペアリング毎に新MACとなり旧ホストの自動奪取を遮断）
    out_addr[5] = id.id[7] + (uint8_t)slot + slot_salts[slot];
}

// ホストが休止・サスペンド等で切断シーケンスに応答しない場合の安全タイムアウト (300ms)
static void disconnect_timeout_handler(btstack_timer_source_t *ts) {
    if (con_handle != HCI_CON_HANDLE_INVALID) {
        printf("[BT] Disconnect timeout (host suspended/unresponsive). Force-closing handle 0x%04X\r\n", con_handle);
        con_handle = HCI_CON_HANDLE_INVALID;
        ble_kbd_ready = false;
        ble_mouse_ready = false;
        ble_encrypted = false;
        ble_can_send = true;
        slot_disconnect_after_zero = false;
        zero_sent_waiting_ack = false;
        current_peer_addr[0] = '\0';

        if (switch_pending) {
            switch_pending = false;
            if (pending_toggle_off) {
                pending_toggle_off = false;
                is_pairing_mode = false;
                gap_advertisements_enable(0);
                current_state = BLE_STATE_STANDBY_SILENT;
                update_ui_for_state(current_state);
                printf("[BT] Slot %d Radio turned OFF (Standby Silent).\r\n", current_slot + 1);
            } else {
                int slot = pending_slot;
                is_pairing_mode = pending_is_pairing;
                btstack_run_loop_remove_timer(&pairing_timeout_timer);
                if (is_pairing_mode) {
                    btstack_run_loop_set_timer_handler(&pairing_timeout_timer, pairing_mode_timeout_handler);
                    btstack_run_loop_set_timer(&pairing_timeout_timer, 60000);
                    btstack_run_loop_add_timer(&pairing_timeout_timer);
                }
                printf("[BT] Applying new slot identity after force disconnect: Slot %d (pairing=%d)\r\n", slot + 1, is_pairing_mode);
                apply_slot_identity(slot);
            }
        }
    }
}

// クールダウン終了後に電波送出（アドバタイズ）を再開するハンドラ
// スロットごとの名前とアドレスを設定する共通ヘルパー
static void adv_step_timer_handler(btstack_timer_source_t *ts) {
    if (con_handle != HCI_CON_HANDLE_INVALID) {
        adv_step = ADV_STEP_IDLE;
        adv_cooldown_active = false;
        return;
    }

    bool is_bonded = is_slot_bonded(current_slot);
    if (!is_pairing_mode && !is_bonded) {
        adv_step = ADV_STEP_IDLE;
        adv_cooldown_active = false;
        gap_advertisements_enable(0);
        current_state = BLE_STATE_STANDBY_SILENT;
        update_ui_for_state(current_state);
        return;
    }

    switch (adv_step) {
        case ADV_STEP_SET_ADDR: {
            bd_addr_t slot_addr;
            get_slot_random_address(current_slot, slot_addr);
            gap_random_address_set_mode(GAP_RANDOM_ADDRESS_TYPE_STATIC);
            gap_random_address_set(slot_addr);

            adv_step = ADV_STEP_SET_DATA;
            btstack_run_loop_set_timer_handler(&adv_step_timer, adv_step_timer_handler);
            btstack_run_loop_set_timer(&adv_step_timer, 150);
            btstack_run_loop_add_timer(&adv_step_timer);
            break;
        }

        case ADV_STEP_SET_DATA: {
            char name_buf[32];
            snprintf(name_buf, sizeof(name_buf), "Remapper [Slot %d]", current_slot + 1);

            uint8_t adv_data[31];
            int adv_data_len = 0;

            adv_data[adv_data_len++] = 2;
            adv_data[adv_data_len++] = BLUETOOTH_DATA_TYPE_FLAGS;
            adv_data[adv_data_len++] = 0x06;

            adv_data[adv_data_len++] = 3;
            adv_data[adv_data_len++] = BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS;
            adv_data[adv_data_len++] = 0x12;
            adv_data[adv_data_len++] = 0x18;

            adv_data[adv_data_len++] = 3;
            adv_data[adv_data_len++] = BLUETOOTH_DATA_TYPE_APPEARANCE;
            adv_data[adv_data_len++] = 0xC2; // 0x03C2: Mouse
            adv_data[adv_data_len++] = 0x03;

            int name_len = strlen(name_buf);
            if (name_len > 18) name_len = 18;
            adv_data[adv_data_len++] = name_len + 1;
            adv_data[adv_data_len++] = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
            memcpy(&adv_data[adv_data_len], name_buf, name_len);
            adv_data_len += name_len;

            gap_advertisements_set_data(adv_data_len, adv_data);

            adv_step = ADV_STEP_ENABLE;
            btstack_run_loop_set_timer_handler(&adv_step_timer, adv_step_timer_handler);
            btstack_run_loop_set_timer(&adv_step_timer, 150);
            btstack_run_loop_add_timer(&adv_step_timer);
            break;
        }

        case ADV_STEP_ENABLE: {
            adv_step = ADV_STEP_IDLE;
            adv_cooldown_active = false;

            if (is_pairing_mode) {
                current_state = BLE_STATE_PAIRING;
            } else {
                current_state = BLE_STATE_RECONNECTING;
            }
            update_ui_for_state(current_state);

            bd_addr_t slot_addr;
            get_slot_random_address(current_slot, slot_addr);
            gap_advertisements_enable(1);
            printf("[BT] <- apply_slot_identity done (state=%d, pairing=%d, bonded=%d). ADDR=%s\r\n",
                   current_state, is_pairing_mode, is_bonded, bd_addr_to_str(slot_addr));
            break;
        }

        default:
            adv_step = ADV_STEP_IDLE;
            adv_cooldown_active = false;
            break;
    }
}

// スロットごとの名前とアドレスを適用してアドバタイズを安全に再設定する共通関数
static void apply_slot_identity(int slot) {
    current_slot = slot;
    save_current_slot((uint8_t)slot); // Flash に保存

    printf("[BT] -> apply_slot_identity(Slot %d, pairing=%d) start\r\n", slot + 1, is_pairing_mode);

    // 既存アドバタイズ・進行中シーケンスを一旦停止
    gap_advertisements_enable(0);
    btstack_run_loop_remove_timer(&adv_step_timer);
    adv_step = ADV_STEP_IDLE;
    adv_cooldown_active = false;

    if (is_pairing_mode) {
        current_state = BLE_STATE_PAIRING;
    } else {
        bool is_bonded = is_slot_bonded(current_slot);
        current_state = is_bonded ? BLE_STATE_RECONNECTING : BLE_STATE_STANDBY_SILENT;
        btstack_run_loop_remove_timer(&reconnect_timeout_timer);
        if (is_bonded) {
            btstack_run_loop_set_timer_handler(&reconnect_timeout_timer, reconnect_timeout_handler);
            btstack_run_loop_set_timer(&reconnect_timeout_timer, 30000); // 30秒タイムアウト
            btstack_run_loop_add_timer(&reconnect_timeout_timer);
        }
    }
    update_ui_for_state(current_state);

    bool is_bonded = is_slot_bonded(current_slot);
    // 【厳格なペアリング制御】未ペアリングかつペアリングモードでない場合は電波を完全停止
    if (!is_pairing_mode && !is_bonded) {
        printf("[BT] Slot %d is unbonded and not in pairing mode. Radio OFF (silent).\r\n", current_slot + 1);
        current_state = BLE_STATE_STANDBY_SILENT;
        update_ui_for_state(current_state);
        return;
    }

    // 各HCIコマンドを150ms間隔で段階的に送信する安全ステートマシンを開始（CYW43 SPI競合・破棄を100%防止）
    adv_step = ADV_STEP_SET_ADDR;
    adv_cooldown_active = true;
    btstack_run_loop_set_timer_handler(&adv_step_timer, adv_step_timer_handler);
    btstack_run_loop_set_timer(&adv_step_timer, 150);
    btstack_run_loop_add_timer(&adv_step_timer);
}

// ホットキーから呼ばれる関数
// 同一スロット押下でスロットのON/OFF（電波OFF・切断 / 再接続）をトグルする
void ble_switch_slot(int slot) {
    if (slot < 0 || slot >= MAX_SLOTS) return;

    if (slot == current_slot) {
        // 同一スロットのトグル操作
        if (con_handle != HCI_CON_HANDLE_INVALID) {
            printf("[BT] Slot %d connected -> Disconnecting to turn Radio OFF\r\n", slot + 1);
            pending_slot = slot;
            pending_is_pairing = false;
            pending_toggle_off = true;
            slot_change_requested = true;
            current_state = BLE_STATE_SWITCHING;
            update_ui_for_state(current_state);
            return;
        } else if (current_state == BLE_STATE_RECONNECTING || current_state == BLE_STATE_PAIRING) {
            printf("[BT] Slot %d active advertising -> Turning Radio OFF\r\n", slot + 1);
            is_pairing_mode = false;
            gap_advertisements_enable(0);
            btstack_run_loop_remove_timer(&adv_step_timer);
            btstack_run_loop_remove_timer(&reconnect_timeout_timer);
            btstack_run_loop_remove_timer(&pairing_timeout_timer);
            current_state = BLE_STATE_STANDBY_SILENT;
            update_ui_for_state(current_state);
            return;
        } else {
            // 電波OFF（SILENT）状態から同一スロットを押下：ボンド情報があれば再接続（ON）
            if (is_slot_bonded(slot)) {
                printf("[BT] Slot %d is OFF -> Turning Radio ON to reconnect\r\n", slot + 1);
                pending_slot = slot;
                pending_is_pairing = false;
                pending_toggle_off = false;
                slot_change_requested = true;
                current_state = BLE_STATE_SWITCHING;
                update_ui_for_state(current_state);
                return;
            } else {
                printf("[BT] Slot %d is unbonded and OFF. Use Pairing Hotkey (Win+Ctrl+Alt+F%d) to pair.\r\n", slot + 1, slot + 1);
                return;
            }
        }
    }

    // 別スロットへの切替
    printf("[BT] Request Switch to Slot %d\r\n", slot + 1);
    pending_slot = slot;
    pending_is_pairing = false;
    pending_toggle_off = false;
    slot_change_requested = true;
    current_state = BLE_STATE_SWITCHING;
    update_ui_for_state(current_state);
}

void ble_hid_switch_host(uint8_t slot) {
    ble_switch_slot((int)slot);
}

void ble_enter_pairing_mode(int slot) {
    if (slot < 0 || slot >= MAX_SLOTS) return;
    printf("[BT] Request Pairing Mode for Slot %d\r\n", slot + 1);

    // 【スロットMACアドレスのローテーション（ソルト加算）】
    // ペアリングモード突入時にスロットのMACアドレスを世代更新し、旧ホストの自動接続を遮断
    slot_salts[slot]++;
    save_slot_salts();

    // 【スロット指定のボンド消去】
    // 該当スロットの登録デバイス (slot_peers[slot].addr) のみを le_device_db から検索して物理削除
    if (slot_peers[slot].valid) {
        int db_count = le_device_db_count();
        for (int i = 0; i < db_count; i++) {
            int a_type = BD_ADDR_TYPE_UNKNOWN;
            bd_addr_t a_addr;
            le_device_db_info(i, &a_type, a_addr, NULL);
            if (memcmp(a_addr, slot_peers[slot].addr, 6) == 0) {
                printf("[BT] Cleared Slot %d bond from le_device_db (index %d, addr %s)\r\n",
                       slot + 1, i, bd_addr_to_str(a_addr));
                gap_delete_bonding((bd_addr_type_t)a_type, a_addr);
                le_device_db_remove(i);
                break;
            }
        }
        slot_peers[slot].valid = 0;
        memset(slot_peers[slot].addr, 0, 6);
        save_slot_peers();
    }

    slot_bonded_mask &= ~(1 << slot);
    save_slot_bonded_mask(slot_bonded_mask);
    oled_set_slot_bonded(slot, false);

    pending_slot = slot;
    pending_is_pairing = true;
    pending_toggle_off = false;
    slot_change_requested = true;
    current_state = BLE_STATE_PAIRING;
    update_ui_for_state(current_state);
}
static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event = hci_event_packet_get_type(packet);

    switch (event) {
        case BTSTACK_EVENT_STATE: {
            uint8_t state = btstack_event_state_get_state(packet);
            printf("[BT] BTSTACK_STATE: %d\r\n", state);
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
                    printf("[BT] Input report enabled for ID %d: %d\r\n", report_id, enabled);

                    // 【BL-15】レポートIDごとの購読状態を厳密に個別管理
                    if (report_id == 2) { // Keyboard (Report ID 2)
                        ble_kbd_ready = enabled;
                    } else if (report_id == 1) { // Mouse (Report ID 1)
                        ble_mouse_ready = enabled;
                    }

                    // 【BL-14/15】暗号化完了 かつ キーボード購読完了で安定化タイマ(100ms)開始後に CONNECTED 遷移
                    check_and_start_connection_stabilize();
                    break;
                }
                case HIDS_SUBEVENT_CAN_SEND_NOW: {
                    ble_can_send = true;
                    btstack_run_loop_remove_timer(&can_send_timeout_timer);
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
                    btstack_run_loop_remove_timer(&conn_stabilize_timer);
                    btstack_run_loop_remove_timer(&adv_step_timer);
                    adv_step = ADV_STEP_IDLE;
                    btstack_run_loop_remove_timer(&reconnect_timeout_timer);
                    adv_cooldown_active = false;
                    con_handle = new_handle;

                    // ハンドシェイク完了安全タイムアウト (10秒)
                    btstack_run_loop_remove_timer(&conn_handshake_timeout_timer);
                    btstack_run_loop_set_timer_handler(&conn_handshake_timeout_timer, conn_handshake_timeout_handler);
                    btstack_run_loop_set_timer(&conn_handshake_timeout_timer, 10000);
                    btstack_run_loop_add_timer(&conn_handshake_timeout_timer);

                    bd_addr_t peer_addr;
                    hci_subevent_le_connection_complete_get_peer_address(packet, peer_addr);

                    snprintf(current_peer_addr, sizeof(current_peer_addr), "%s", bd_addr_to_str(peer_addr));
                    printf("[BT] Physical link connected: Slot %d (peer=%s)\r\n", current_slot + 1, current_peer_addr);

                    // 【BL-15】物理リンク接続時点ではまだ送信禁止（CONNECTING）
                    ble_kbd_ready = false;
                    ble_mouse_ready = false;
                    ble_encrypted = false;
                    ble_can_send = true;
                    slot_disconnect_after_zero = false;
                    zero_sent_waiting_ack = false;
                    gap_advertisements_enable(0);

                    current_state = BLE_STATE_CONNECTING;
                    update_ui_for_state(current_state);

                    // Central (Mac/Win/iOS) 側からのペアリング/暗号化要求を待機（Security Request 重複衝突を防止）
                }
            } else if (subevent == HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE) {
                uint16_t interval = hci_subevent_le_connection_update_complete_get_conn_interval(packet);
                uint16_t latency = hci_subevent_le_connection_update_complete_get_conn_latency(packet);
                uint16_t timeout = hci_subevent_le_connection_update_complete_get_supervision_timeout(packet);
                printf("[BT] Connection parameters updated: interval=%.2fms, latency=%d, timeout=%dms\r\n",
                       interval * 1.25f, latency, timeout * 10);
            }
            break;
        }

        // Security Manager (SM) イベントの処理
        case SM_EVENT_JUST_WORKS_REQUEST: {
            hci_con_handle_t handle = sm_event_just_works_request_get_handle(packet);
            printf("[BT] SM_EVENT_JUST_WORKS_REQUEST for handle=0x%04X -> Confirming\r\n", handle);
            sm_just_works_confirm(handle);
            break;
        }

        case SM_EVENT_NUMERIC_COMPARISON_REQUEST: {
            hci_con_handle_t handle = sm_event_numeric_comparison_request_get_handle(packet);
            printf("[BT] SM_EVENT_NUMERIC_COMPARISON_REQUEST for handle=0x%04X -> Confirming\r\n", handle);
            sm_numeric_comparison_confirm(handle);
            break;
        }

        case SM_EVENT_PAIRING_STARTED:
            printf("[BT] SM_EVENT_PAIRING_STARTED\r\n");
            oled_set_bt_state(OLED_BT_PAIRING);
            oled_set_status("Pairing...");
            break;

        case SM_EVENT_PAIRING_COMPLETE: {
            uint8_t status = sm_event_pairing_complete_get_status(packet);
            printf("[BT] SM_EVENT_PAIRING_COMPLETE status=0x%02X\r\n", status);
            if (status == ERROR_CODE_SUCCESS) {
                // 新規ペアリングが成功したらペアリングモードを終了し、ボンド状態を保存
                is_pairing_mode = false;
                btstack_run_loop_remove_timer(&pairing_timeout_timer);
                slot_bonded_mask |= (1 << current_slot);
                oled_set_slot_bonded(current_slot, true);
                printf("[BT] Pairing complete, total bonded devices: %d, mask=0x%02X\r\n", le_device_db_count(), slot_bonded_mask);

                // ペアリング完了したホストの登録情報をメモリ上に記録（Flash保存は接続安定化ハンドラで実行）
                int newest_idx = le_device_db_count() - 1;
                if (newest_idx >= 0) {
                    int a_type = BD_ADDR_TYPE_UNKNOWN;
                    bd_addr_t a_addr;
                    le_device_db_info(newest_idx, &a_type, a_addr, NULL);
                    if (a_type != BD_ADDR_TYPE_UNKNOWN) {
                        slot_peers[current_slot].valid = 1;
                        slot_peers[current_slot].addr_type = a_type;
                        memcpy(slot_peers[current_slot].addr, a_addr, 6);
                        printf("[BT] Recorded Slot %d peer in DB: type=%d, addr=%s\r\n",
                               current_slot + 1, a_type, bd_addr_to_str(a_addr));
                    }
                }

                check_and_start_connection_stabilize();
            }
            break;
        }

        // 既知ホストの再接続検知
        case SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED: {
            hci_con_handle_t handle = sm_event_identity_resolving_succeeded_get_handle(packet);
            int device_index = sm_event_identity_resolving_succeeded_get_index(packet);
            printf("[BT] Identity resolved: handle=0x%04X, device_index=%d\r\n", handle, device_index);
            // 既知ホストの接続を0msで強制切断しない（再接続無限ループによるホストBTクラッシュの完全防止）
            break;
        }

        case HCI_EVENT_ENCRYPTION_CHANGE: {
            uint8_t status = hci_event_encryption_change_get_status(packet);
            uint8_t enabled = hci_event_encryption_change_get_encryption_enabled(packet);
            if (status == 0 && enabled) {
                printf("[BT] Encryption enabled\r\n");
                ble_encrypted = true;

                // 暗号化成功＝このスロットに有効なボンドが存在することを確定
                if (!(slot_bonded_mask & (1 << current_slot))) {
                    slot_bonded_mask |= (1 << current_slot);
                    oled_set_slot_bonded(current_slot, true);
                    printf("[BT] Confirmed Slot %d bonded via encryption, mask=0x%02X\r\n", current_slot + 1, slot_bonded_mask);
                }

                // 【BL-14/15】暗号化完了 かつ キーボード購読完了で安定化タイマ(100ms)開始後に CONNECTED 遷移
                check_and_start_connection_stabilize();
            } else if (status != 0) {
                printf("[BT] Encryption failed (status=0x%02X). Disconnecting.\r\n", status);
                ble_encrypted = false;
                if (con_handle != HCI_CON_HANDLE_INVALID) {
                    gap_disconnect(con_handle);
                }
            }
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            if (con_handle == HCI_CON_HANDLE_INVALID) {
                // 重複イベント（hids_device と hci_event_callback の多重登録）を無視
                break;
            }
            btstack_run_loop_remove_timer(&disconnect_timeout_timer);
            btstack_run_loop_remove_timer(&conn_stabilize_timer);
            btstack_run_loop_remove_timer(&conn_handshake_timeout_timer);
            con_handle = HCI_CON_HANDLE_INVALID;
            ble_kbd_ready = false;
            ble_mouse_ready = false;
            ble_encrypted = false;
            btstack_run_loop_remove_timer(&can_send_timeout_timer);
            ble_can_send = true;
            slot_disconnect_after_zero = false;
            zero_sent_waiting_ack = false;
            current_peer_addr[0] = '\0';

            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            printf("[BT] Disconnected (reason=0x%02X)\r\n", reason);

            // スロット切り替え待ちがある場合、切断完了後に安全に新スロットを適用して再開
            if (switch_pending) {
                switch_pending = false;
                if (pending_toggle_off) {
                    pending_toggle_off = false;
                    is_pairing_mode = false;
                    gap_advertisements_enable(0);
                    current_state = BLE_STATE_STANDBY_SILENT;
                    update_ui_for_state(current_state);
                    printf("[BT] Slot %d Radio turned OFF (Standby Silent).\r\n", current_slot + 1);
                } else {
                    int slot = pending_slot;
                    is_pairing_mode = pending_is_pairing;
                    btstack_run_loop_remove_timer(&pairing_timeout_timer);
                    if (is_pairing_mode) {
                        btstack_run_loop_set_timer_handler(&pairing_timeout_timer, pairing_mode_timeout_handler);
                        btstack_run_loop_set_timer(&pairing_timeout_timer, 60000);
                        btstack_run_loop_add_timer(&pairing_timeout_timer);
                    }
                    printf("[BT] Applying new slot identity after disconnect: Slot %d (pairing=%d)\r\n", slot + 1, is_pairing_mode);
                    apply_slot_identity(slot);
                }
            } else {
                bool is_bonded = is_slot_bonded(current_slot);
                if (is_pairing_mode) {
                    current_state = BLE_STATE_PAIRING;
                } else if (is_bonded) {
                    current_state = BLE_STATE_RECONNECTING;
                    btstack_run_loop_remove_timer(&reconnect_timeout_timer);
                    btstack_run_loop_set_timer_handler(&reconnect_timeout_timer, reconnect_timeout_handler);
                    btstack_run_loop_set_timer(&reconnect_timeout_timer, 30000);
                    btstack_run_loop_add_timer(&reconnect_timeout_timer);
                } else {
                    current_state = BLE_STATE_STANDBY_SILENT;
                    btstack_run_loop_remove_timer(&reconnect_timeout_timer);
                }
                update_ui_for_state(current_state);

                // 切断直後はホスト側 (macOS等) のドライバ破棄猶予を確保するため、
                // 500ms の静音期間をおいてからアドバタイズを再開する（再接続ストーム・ホストBTクラッシュの完全防止）
                // 切断直後は 150ms 毎のステートマシンで安全に電波再開
                if (current_state != BLE_STATE_STANDBY_SILENT) {
                    apply_slot_identity(current_slot);
                } else {
                    gap_advertisements_enable(0);
                }
            }
            break;
        }

        default:
            break;
    }
}

void ble_hid_init(void) {
    printf("[BT] BLE HID Stack init\r\n");
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

    l2cap_init();

    // 3. ペアリング鍵 DB に TLV を接続（先に DB をロードする）
    le_device_db_tlv_configure(tlv_impl, &tlv_context);

    // 4. SM の初期化
    sm_init();

    // 5. DB ロード完了後にスロット番号とボンド情報を復元（DBエントリのマイグレーション含む）
    load_current_slot();
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
    // 高速アドバタイズ間隔 (30ms = 0x0030) に設定
    // 初期化時に一括設定し、実行中の再設定（HCI競合やCommand Disallowed）を防止。
    // 30ms間隔により、Windows 11 や macOS の低デューティスキャンでも確実に検出可能。
    // 静的ランダムアドレスモードを初期化し、own_address_type=Random (1) を確定させる
    bd_addr_t init_addr;
    get_slot_random_address(current_slot, init_addr);
    gap_random_address_set_mode(GAP_RANDOM_ADDRESS_TYPE_STATIC);
    gap_random_address_set(init_addr);

    uint16_t adv_int_min = 0x0030; // 30 ms
    uint16_t adv_int_max = 0x0030; // 30 ms
    bd_addr_t null_addr;
    memset(null_addr, 0, 6);
    gap_advertisements_set_params(adv_int_min, adv_int_max, 0, 0, null_addr, 0x07, 0x00);

    current_state = BLE_STATE_STANDBY_SILENT;
    update_ui_for_state(current_state);

    hci_power_control(HCI_POWER_ON);
}


void ble_hid_task(void) {
    if (slot_change_requested) {
        slot_change_requested = false;
        int slot = pending_slot;
        bool is_pairing = pending_is_pairing;

        printf("[BT] Processing slot change to %d (pairing=%d)\r\n", slot + 1, is_pairing);

        // 既存接続がある場合は、必ずホストへ全キー離下（zero_kbd）を届けて完了を確認してから切断する
        if (con_handle != HCI_CON_HANDLE_INVALID) {
            slot_disconnect_after_zero = true;
            zero_sent_waiting_ack = false;
            switch_pending = true;
            current_state = BLE_STATE_SWITCHING;
            update_ui_for_state(current_state);

            // ホストが休止・サスペンド等で応答しない場合の安全タイムアウト (300ms)
            btstack_run_loop_remove_timer(&disconnect_timeout_timer);
            btstack_run_loop_set_timer_handler(&disconnect_timeout_timer, disconnect_timeout_handler);
            btstack_run_loop_set_timer(&disconnect_timeout_timer, 300);
            btstack_run_loop_add_timer(&disconnect_timeout_timer);

            if (ble_can_send && ble_encrypted && ble_kbd_ready) {
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
                // 送信準備が整っていない状態での切断要求なら即時切断
                slot_disconnect_after_zero = false;
                gap_disconnect(con_handle);
            }
        } else {
            // 未接続状態で別スロットへの切り替え、またはペアリングモード開始
            switch_pending = false;
            is_pairing_mode = is_pairing;
            btstack_run_loop_remove_timer(&pairing_timeout_timer);
            if (is_pairing) {
                btstack_run_loop_set_timer_handler(&pairing_timeout_timer, pairing_mode_timeout_handler);
                btstack_run_loop_set_timer(&pairing_timeout_timer, 60000);
                btstack_run_loop_add_timer(&pairing_timeout_timer);
            }
            apply_slot_identity(slot);
        }
    }
}

bool ble_hid_is_connected(void) {
    return current_state == BLE_STATE_CONNECTED;
}

bool ble_hid_can_send(void) {
    return (current_state == BLE_STATE_CONNECTED) &&
           (con_handle != HCI_CON_HANDLE_INVALID) &&
           ble_can_send &&
           !slot_disconnect_after_zero;
}

void ble_hid_send_report(uint8_t report_id, const uint8_t* data, uint8_t len) {
    if (current_state != BLE_STATE_CONNECTED || con_handle == HCI_CON_HANDLE_INVALID) {
        return;
    }

    // 【BL-15】暗号化完了 かつ 該当レポート購読完了を厳密にチェック
    if (!ble_encrypted) return;
    if (report_id == 2 && !ble_kbd_ready) return;    // Report ID 2: Keyboard
    if (report_id == 1 && !ble_mouse_ready) return;  // Report ID 1: Mouse

    if (!ble_can_send) {
        return;
    }

    oled_notify_tx();
    uint8_t status = hids_device_send_input_report_for_id(con_handle, report_id, (uint8_t*)data, len);
    if (status == ERROR_CODE_SUCCESS || status == 0x57) { // 0x57: BTSTACK_ACL_BUFFERS_FULL
        ble_can_send = false;
        btstack_run_loop_remove_timer(&can_send_timeout_timer);
        btstack_run_loop_set_timer_handler(&can_send_timeout_timer, can_send_timeout_handler);
        btstack_run_loop_set_timer(&can_send_timeout_timer, 2000); // 2秒の安全ウォッチドッグ
        btstack_run_loop_add_timer(&can_send_timeout_timer);
        hids_device_request_can_send_now_event(con_handle);
    } else {
        printf("[BLE-ERR] send_report id=%d len=%d status=0x%02X\r\n", report_id, len, status);
        ble_can_send = true;
    }
}


void ble_hid_kick_reconnect(void) {
    if (adv_cooldown_active) {
        return;
    }
    if (current_state == BLE_STATE_RECONNECTING && con_handle == HCI_CON_HANDLE_INVALID) {
        static uint32_t last_kick_ms = 0;
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_kick_ms > 2000) {
            last_kick_ms = now;
            printf("[BT] Activity detected in RECONNECTING state. Re-asserting advertisement.\r\n");
            gap_advertisements_enable(1);
            btstack_run_loop_remove_timer(&reconnect_timeout_timer);
            btstack_run_loop_set_timer_handler(&reconnect_timeout_timer, reconnect_timeout_handler);
            btstack_run_loop_set_timer(&reconnect_timeout_timer, 30000);
            btstack_run_loop_add_timer(&reconnect_timeout_timer);
        }
    }
}

void ble_pair_new_device(void) {
    ble_enter_pairing_mode(current_slot);
}

void ble_clear_bonds(void) {
    int count = le_device_db_count();
    for (int i = count - 1; i >= 0; i--) {
        int addr_type = BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t addr;
        le_device_db_info(i, &addr_type, addr, NULL);
        if (addr_type != BD_ADDR_TYPE_UNKNOWN) {
            gap_delete_bonding((bd_addr_type_t)addr_type, addr);
        }
        le_device_db_remove(i);
    }
    save_slot_bonded_mask(0);
    memset(slot_peers, 0, sizeof(slot_peers));
    save_slot_peers();
    memset(slot_salts, 0, sizeof(slot_salts));
    save_slot_salts();
    printf("[BT] Cleared all %d bonds from DB and Resolving List\r\n", count);
    for (int i = 0; i < 4; i++) {
        oled_set_slot_bonded(i, false);
    }
    current_state = BLE_STATE_STANDBY_SILENT;
    update_ui_for_state(current_state);
    oled_set_status("Bonds Cleared");
    gap_advertisements_enable(0);
    ble_led(0);
}

void ble_led(bool value) {
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, value);

}

#endif // ENABLE_BLE