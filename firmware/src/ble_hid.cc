#if ENABLE_BLE

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/async_context.h"
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
#include "remote_log.h"


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
// スロットごとのOS設定/自動判定結果を保存する独自タグ ID ('S' 'O' 'S' '1')
#define TLV_TAG_SLOT_OS 0x534f5331
// BT-OFF（完全沈黙/STANDBY_SILENT）状態を保存する独自タグ ID ('R' 'O' 'F' 'F')
#define TLV_TAG_RADIO_OFF 0x524f4646

#define MAX_SLOTS NVM_NUM_DEVICE_DB_ENTRIES

typedef struct {
    uint8_t valid;
    uint8_t addr_type;
    bd_addr_t addr;
} slot_peer_t;

static uint8_t slot_bonded_mask = 0;
static slot_peer_t slot_peers[MAX_SLOTS];
static uint8_t slot_salts[MAX_SLOTS];
static char slot_os[MAX_SLOTS] = {'-', '-', '-', '-'};
static bool radio_off_saved = false;
static uint16_t ble_conn_latency = 0;
static uint16_t ble_conn_timeout = 0;
static bool peer_has_android_timeout = false;


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

    // --- Consumer Control (Report ID 3: Media Keys) ---
    0x05, 0x0C,                    // Usage Page (Consumer)
    0x09, 0x01,                    // Usage (Consumer Control)
    0xA1, 0x01,                    // Collection (Application)
    0x85, 0x03,                    //   Report ID (3)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x01,                    //   Logical Maximum (1)
    0x09, 0xB5,                    //   Usage (Scan Next Track)
    0x09, 0xB6,                    //   Usage (Scan Previous Track)
    0x09, 0xB7,                    //   Usage (Stop)
    0x09, 0xCD,                    //   Usage (Play/Pause)
    0x09, 0xE2,                    //   Usage (Mute)
    0x09, 0xE9,                    //   Usage (Volume Increment)
    0x09, 0xEA,                    //   Usage (Volume Decrement)
    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x07,                    //   Report Count (7)
    0x81, 0x02,                    //   Input (Data,Var,Abs)
    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x03,                    //   Input (Const,Var,Abs)
    0xC0,                          // End Collection
};

static hids_device_report_t hid_reports[3];

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
static bool ble_consumer_ready = false; // Report ID 3 (Consumer Control) subscribed
static bool conn_stabilize_active = false;
static bool ble_encrypted = false;      // Link encrypted
static bool ble_can_send = true;
static bool slot_disconnect_after_zero = false;
static bool zero_sent_waiting_ack = false;
static char current_peer_addr[18] = "";

static uint16_t ble_conn_interval = 0;

uint16_t ble_hid_get_conn_interval(void) {
    return ble_conn_interval;
}

// BTstack / CYW43 非同期コンテキスト用排他ロック
static inline void ble_lock(void) {
    async_context_t *context = cyw43_arch_async_context();
    if (context) {
        async_context_acquire_lock_blocking(context);
    }
}

static inline void ble_unlock(void) {
    async_context_t *context = cyw43_arch_async_context();
    if (context) {
        async_context_release_lock(context);
    }
}

static int consecutive_can_send_timeouts = 0;

// 広告の有効化は「コマンドは投げたがコントローラに拒否された (0x0C 等)」場合に
// 無言で失敗し、ペアリングモードなのに誰からも見えない状態になる (BL-45)。
// 要求値と受理結果を保持し、未達なら ble_hid_task 側で張り直す
static volatile int adv_enable_req = 0;
static volatile bool adv_enabled_ok = false;
static uint64_t adv_retry_next_us = 0;

static void adv_set(int enable) {
    adv_enable_req = enable;
    gap_advertisements_enable(enable);
}


static int current_slot = 0;

uint32_t ble_hid_get_connected_slot_mask(void) {
    return (current_state == BLE_STATE_CONNECTED) ? (1u << current_slot) : 0;
}

// BL-43/45 ウェッジ観測用カウンタ。リアルタイム経路では加算のみ行う
static volatile uint32_t health_ok = 0;
static volatile uint32_t health_acl_full = 0;
static volatile uint32_t health_blocked = 0;
static volatile uint32_t health_disc = 0;
static volatile uint8_t health_disc_reason = 0;
static volatile uint64_t health_last_ok_us = 0;

void ble_hid_get_health(ble_hid_health_t* out) {
    if (out == nullptr) return;
    out->ci = ble_hid_get_conn_interval();
    out->ok = health_ok;
    out->acl_full = health_acl_full;
    out->blocked = health_blocked;
    out->disc = health_disc;
    out->disc_reason = health_disc_reason;
    out->last_ok_us = health_last_ok_us;
}

static bool is_pairing_mode = false;
static btstack_timer_source_t pairing_timeout_timer;
static btstack_timer_source_t disconnect_timeout_timer;
static btstack_timer_source_t conn_stabilize_timer;
static btstack_timer_source_t can_send_timeout_timer;
static btstack_timer_source_t reconnect_timeout_timer;
static uint64_t reconnect_start_us = 0;
static btstack_timer_source_t conn_handshake_timeout_timer;
static hci_con_handle_t con_handle = HCI_CON_HANDLE_INVALID;
static btstack_timer_source_t slot_transition_timer;
static int slot_transition_target = -1;
static bool slot_transition_pairing = false;
static uint64_t pairing_start_us = 0;
static int last_created_device_index = -1;
static bool bl17_reject_in_progress = false;
static btstack_timer_source_t bl17_cooldown_timer;
static void bl17_cooldown_timer_handler(btstack_timer_source_t *ts);
static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;

static btstack_tlv_flash_bank_t tlv_context;
static const btstack_tlv_t *tlv_impl = NULL;

static volatile bool slot_change_requested = false;
static volatile int pending_slot = -1;
static volatile bool pending_is_pairing = false;
static volatile bool pending_toggle_off = false;
static volatile bool switch_pending = false;

// スロット切替・ON/OFF トグルの受付禁止時間 (3 秒)。切替直後の連続操作による
// BT 状態の不整合を防ぐ保険。全消去・ボンド消去・ペアリングは対象外
#define SWITCH_LOCK_US (3000000ULL)
static uint64_t switch_lock_until_us = 0;



static void apply_slot_identity(int slot);
static void update_detected_os(char os);
static void save_slot_timer_handler(btstack_timer_source_t *ts);
static void schedule_save_slot_os(void);
static void schedule_save_radio_off(bool is_off);
static void update_ui_for_state(ble_state_t state);
static void save_slot_bonded_mask(uint8_t mask);
static void save_slot_peers(void);
static void ble_switch_slot_locked(int slot);
static void ble_enter_pairing_mode_locked(int slot);
static void ble_clear_bonds_locked(void);
static void ble_hid_task_locked(void);
static void ble_hid_kick_reconnect_locked(void);

static void update_connecting_status(void) {
#if ENABLE_OLED
    if (current_state == BLE_STATE_CONNECTING) {
        char stat_buf[20];
        snprintf(stat_buf, sizeof(stat_buf), "C:E%d K%d M%d",
                 ble_encrypted ? 1 : 0,
                 ble_kbd_ready ? 1 : 0,
                 ble_mouse_ready ? 1 : 0);
        oled_set_status(stat_buf);
    }
#endif
}

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
            update_connecting_status();
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
        consecutive_can_send_timeouts++;
        printf("[BT] WARNING: CAN_SEND_NOW watchdog timed out (2000ms, count=%d). Resetting flow control.\r\n",
               consecutive_can_send_timeouts);
        remote_log_eventf("CANSEND-TO n=%d", consecutive_can_send_timeouts);

        // 2回連続（4秒間）完了通知が途絶えた場合はBTstack/CYW43バッファの完全膠着と判定
        // ゾンビ状態での永久停止を防ぎ、クリーンに切断して自動再接続シーケンスへ移行
        if (consecutive_can_send_timeouts >= 2) {
            printf("[BT] CRITICAL: BLE transmission deadlocked for 4000ms. Forcing link disconnect & clean recovery.\r\n");
            remote_log_eventf("FLOWCTL-DEADLOCK force-disc");
            consecutive_can_send_timeouts = 0;
            ble_can_send = true;
            gap_disconnect(con_handle);
            return;
        }

        ble_can_send = true;
        hids_device_request_can_send_now_event(con_handle);
    }
}

static void reconnect_timeout_handler(btstack_timer_source_t *ts) {
    if (con_handle == HCI_CON_HANDLE_INVALID && current_state == BLE_STATE_RECONNECTING) {
        printf("[BT] Reconnection timeout (30s expired). Radio OFF.\r\n");
        remote_log_eventf("RECONN-TO radio-off");
        adv_set(0);
        current_state = BLE_STATE_STANDBY_SILENT;
        update_ui_for_state(current_state);
        oled_set_status("Ready");
    }
}

static void conn_handshake_timeout_handler(btstack_timer_source_t *ts) {
    if (con_handle != HCI_CON_HANDLE_INVALID && current_state == BLE_STATE_CONNECTING) {
        printf("[BT] Handshake timeout (10s expired without encryption/subscription). Disconnecting stalled link.\r\n");
        remote_log_eventf("HS-TO 10s force-disc");
        gap_disconnect(con_handle);
    }
}

static void conn_stabilize_timer_handler(btstack_timer_source_t *ts) {
    conn_stabilize_active = false;
    if (con_handle != HCI_CON_HANDLE_INVALID && ble_kbd_ready && ble_encrypted) {
        printf("[BT] Connection stabilized -> CONNECTED (Ready to send)\r\n");
        remote_log_eventf("CONNECTED slot=%d", current_slot + 1);
        btstack_run_loop_remove_timer(&conn_handshake_timeout_timer);
        current_state = BLE_STATE_CONNECTED;
        update_ui_for_state(current_state);

        // 【BL-04】接続安定化時のフォールバックOS判定
        uint16_t mtu = att_server_get_mtu(con_handle);
        if (mtu >= 200) {
            if (peer_has_android_timeout || ble_conn_timeout >= 450) {
                update_detected_os('A');
            } else {
                update_detected_os('W');
            }
        } else if (ble_conn_latency > 0) {
            update_detected_os('M');
        }

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
        if (current_state == BLE_STATE_CONNECTED || conn_stabilize_active) {
            return;
        }
        conn_stabilize_active = true;
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

static void load_slot_os(void) {
    if (!tlv_impl) return;
    char saved_os[MAX_SLOTS];
    int len = tlv_impl->get_tag(&tlv_context, TLV_TAG_SLOT_OS, (uint8_t*)saved_os, sizeof(saved_os));
    if (len == sizeof(saved_os)) {
        for (int i = 0; i < MAX_SLOTS; i++) {
            char c = saved_os[i];
            if (c == 'M' || c == 'W' || c == 'A' || c == 'I' || c == '-') {
                slot_os[i] = c;
            } else {
                slot_os[i] = '-';
            }
            oled_set_slot_os(i, slot_os[i]);
        }
        printf("[BT] Restored slot_os: [%c, %c, %c, %c] from TLV\r\n",
               slot_os[0], slot_os[1], slot_os[2], slot_os[3]);
    } else {
        printf("[BT] No saved slot_os found (len=%d), initializing defaults\r\n", len);
        for (int i = 0; i < MAX_SLOTS; i++) {
            slot_os[i] = '-';
            oled_set_slot_os(i, '-');
        }
    }
}

static void load_radio_off(void) {
    if (!tlv_impl) return;
    uint8_t saved_val = 0;
    int len = tlv_impl->get_tag(&tlv_context, TLV_TAG_RADIO_OFF, &saved_val, sizeof(saved_val));
    if (len == sizeof(saved_val)) {
        radio_off_saved = (saved_val != 0);
        printf("[BT] Restored radio_off: %d from TLV\r\n", radio_off_saved);
    } else {
        radio_off_saved = false;
    }
}

static void update_detected_os(char os) {
    if (current_slot < 0 || current_slot >= MAX_SLOTS) return;
    // ユーザーが手動で 'I' (iOS) を指定している場合、Apple系 ('M') 検出で上書きしない
    if (slot_os[current_slot] == 'I' && os == 'M') {
        return;
    }
    // 一度 Apple ('M' または 'I') と判定された場合、低遅延パラメータ(latency=0)再ネゴシエーション等で 'W' や 'A' に上書きしない
    if ((slot_os[current_slot] == 'M' || slot_os[current_slot] == 'I') && (os == 'W' || os == 'A')) {
        return;
    }
    // 一度 Android ('A') と判定された場合、本機側の低遅延パラメータ(timeout=400)再ネゴシエーション等で 'W' に上書きしない
    if (slot_os[current_slot] == 'A' && os == 'W') {
        return;
    }
    if (slot_os[current_slot] != os) {
        printf("[BT] Slot %d OS detected/updated: '%c' -> '%c'\r\n",
               current_slot + 1, slot_os[current_slot], os);
        slot_os[current_slot] = os;
        oled_set_slot_os(current_slot, os);
        schedule_save_slot_os(); // 2秒安定後にFlashへ遅延保存（SPI通信中Flash競合防止）
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
    load_slot_os();
    load_radio_off();
    oled_set_radio_off(radio_off_saved);

    int num_bonded = le_device_db_count();
    int max_db_entries = le_device_db_max_count();

    // 1) slot_peers に記録があり、かつ DB に該当アドレスが存在するか確認
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (slot_peers[i].valid) {
            bool found = false;
            for (int j = 0; j < max_db_entries; j++) {
                int a_type = BD_ADDR_TYPE_UNKNOWN;
                bd_addr_t a_addr;
                le_device_db_info(j, &a_type, a_addr, NULL);
                if (a_type != BD_ADDR_TYPE_UNKNOWN && memcmp(a_addr, slot_peers[i].addr, 6) == 0) {
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
        for (int j = 0; j < max_db_entries; j++) {
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
        oled_set_slot_os(i, slot_os[i]);
    }
}
static btstack_timer_source_t save_slot_timer;
static uint8_t pending_save_slot = 0xFF;
static bool pending_save_os = false;
static bool pending_save_radio_off = false;

static void schedule_save_tlv_timer(void) {
    if (!tlv_impl) return;
    btstack_run_loop_remove_timer(&save_slot_timer);
    btstack_run_loop_set_timer_handler(&save_slot_timer, save_slot_timer_handler);
    btstack_run_loop_set_timer(&save_slot_timer, 2000); // 2秒安定後に安全にFlash保存
    btstack_run_loop_add_timer(&save_slot_timer);
}

static void schedule_save_current_slot(uint8_t slot) {
    if (!tlv_impl || slot >= MAX_SLOTS) return;
    pending_save_slot = slot;
    schedule_save_tlv_timer();
}

static void schedule_save_slot_os(void) {
    if (!tlv_impl) return;
    pending_save_os = true;
    schedule_save_tlv_timer();
}

static void schedule_save_radio_off(bool is_off) {
    if (!tlv_impl) return;
    radio_off_saved = is_off;
    pending_save_radio_off = true;
    schedule_save_tlv_timer();
}

static void save_slot_timer_handler(btstack_timer_source_t *ts) {
    if (!tlv_impl) return;

    if (pending_save_slot < MAX_SLOTS) {
        uint8_t slot_to_save = pending_save_slot;
        pending_save_slot = 0xFF;

        uint8_t current_saved = 0xFF;
        int len = tlv_impl->get_tag(&tlv_context, TLV_TAG_CURRENT_SLOT, &current_saved, sizeof(current_saved));
        if (len != sizeof(current_saved) || current_saved != slot_to_save) {
            int res = tlv_impl->store_tag(&tlv_context, TLV_TAG_CURRENT_SLOT, &slot_to_save, sizeof(slot_to_save));
            printf("[BT] Saved current_slot: %d to TLV (res=%d)\r\n", slot_to_save + 1, res);
        }
    }

    if (pending_save_os) {
        pending_save_os = false;
        char saved_os[MAX_SLOTS];
        int len = tlv_impl->get_tag(&tlv_context, TLV_TAG_SLOT_OS, (uint8_t*)saved_os, sizeof(saved_os));
        if (len != sizeof(saved_os) || memcmp(saved_os, slot_os, sizeof(slot_os)) != 0) {
            int res = tlv_impl->store_tag(&tlv_context, TLV_TAG_SLOT_OS, (const uint8_t*)slot_os, sizeof(slot_os));
            printf("[BT] Saved slot_os: [%c, %c, %c, %c] to TLV (res=%d)\r\n",
                   slot_os[0], slot_os[1], slot_os[2], slot_os[3], res);
        }
    }

    if (pending_save_radio_off) {
        pending_save_radio_off = false;
        uint8_t val = radio_off_saved ? 1 : 0;
        uint8_t current_val = 0xFF;
        int len = tlv_impl->get_tag(&tlv_context, TLV_TAG_RADIO_OFF, &current_val, sizeof(current_val));
        if (len != sizeof(current_val) || current_val != val) {
            int res = tlv_impl->store_tag(&tlv_context, TLV_TAG_RADIO_OFF, &val, sizeof(val));
            printf("[BT] Saved radio_off: %d to TLV (res=%d)\r\n", val, res);
        }
    }
}

// タイムアウト時のハンドラ（ペアリングモード終了）
static void pairing_mode_timeout_handler(btstack_timer_source_t *ts) {
    btstack_run_loop_remove_timer(&bl17_cooldown_timer);
    bl17_reject_in_progress = false;
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

// 【BL-32】切断完了後の CYW43 内部リソース解放猶予タイマ（150ms）
// 切断直後に連続して HCI コマンド（アドレス変更・広告開始）を投げると
// コントローラ側でドロップして電波不発となる問題を確実に防止する
static void slot_transition_timer_handler(btstack_timer_source_t *ts) {
    if (slot_transition_target >= 0 && slot_transition_target < MAX_SLOTS) {
        int slot = slot_transition_target;
        bool is_pairing = slot_transition_pairing;
        slot_transition_target = -1;

        is_pairing_mode = is_pairing;
        if (is_pairing_mode) {
            pairing_start_us = time_us_64(); // カウントダウンは実際に適用された時点から
        }
        btstack_run_loop_remove_timer(&pairing_timeout_timer);
        if (is_pairing_mode) {
            btstack_run_loop_set_timer_handler(&pairing_timeout_timer, pairing_mode_timeout_handler);
            btstack_run_loop_set_timer(&pairing_timeout_timer, 60000);
            btstack_run_loop_add_timer(&pairing_timeout_timer);
        }
        printf("[BT] Slot transition margin (150ms) complete -> apply_slot_identity(Slot %d, pairing=%d)\r\n",
               slot + 1, is_pairing_mode);
        remote_log_eventf("SLOT-APPLY slot=%d pair=%d", slot + 1, is_pairing_mode);
        apply_slot_identity(slot);
    }
}

static void schedule_slot_transition(int slot, bool is_pairing) {
    slot_transition_target = slot;
    slot_transition_pairing = is_pairing;
    printf("[BT] Starting 150ms slot transition margin: Slot %d (pairing=%d)\r\n", slot + 1, is_pairing);
    remote_log_eventf("SLOT-MARGIN slot=%d pair=%d", slot + 1, is_pairing);
    btstack_run_loop_remove_timer(&slot_transition_timer);
    btstack_run_loop_set_timer_handler(&slot_transition_timer, slot_transition_timer_handler);
    btstack_run_loop_set_timer(&slot_transition_timer, 150);
    btstack_run_loop_add_timer(&slot_transition_timer);
}

// ホストが休止・サスペンド等で切断シーケンスに応答しない場合の安全タイムアウト (300ms)
static void disconnect_timeout_handler(btstack_timer_source_t *ts) {
    if (con_handle != HCI_CON_HANDLE_INVALID) {
        printf("[BT] Disconnect timeout (host suspended/unresponsive). Force-closing handle 0x%04X\r\n", con_handle);
        remote_log_eventf("DISC-TO force-close");
        con_handle = HCI_CON_HANDLE_INVALID;
        ble_kbd_ready = false;
        ble_mouse_ready = false;
        ble_consumer_ready = false;
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
                adv_set(0);
                current_state = BLE_STATE_STANDBY_SILENT;
                update_ui_for_state(current_state);
                schedule_save_radio_off(true); // BL-01: 2秒安定後にFlash保存
                printf("[BT] Slot %d Radio turned OFF (Standby Silent).\r\n", current_slot + 1);
            } else {
                int slot = pending_slot;
                bool is_pairing = pending_is_pairing;
                printf("[BT] Force disconnect complete: Slot %d (pairing=%d)\r\n", slot + 1, is_pairing);
                schedule_slot_transition(slot, is_pairing);
            }
        }
    }
}

// BL-17 ガード発動後のペアリング広告再開ハンドラ（ホストの再接続ストーム収束後に電波再開）
static void bl17_cooldown_timer_handler(btstack_timer_source_t *ts) {
    if (con_handle == HCI_CON_HANDLE_INVALID && is_pairing_mode) {
        printf("[BT] BL-17 Cooldown expired (2500ms). Resuming pairing advertisements for Slot %d\r\n", current_slot + 1);
        remote_log_eventf("BL17-COOL resume-adv");
        apply_slot_identity(current_slot);
    }
}

// スロットごとの名前とアドレスを適用してアドバタイズを即時・確実に再設定する共通関数
static void apply_slot_identity(int slot) {
    current_slot = slot;
    schedule_save_current_slot((uint8_t)slot); // 2秒後にFlashへ遅延保存（連続切替時のFlash競合・摩耗防止）

    printf("[BT] -> apply_slot_identity(Slot %d, pairing=%d) start\r\n", slot + 1, is_pairing_mode);

    // 既存アドバタイズを一旦停止
    adv_set(0);

    bool is_bonded = is_slot_bonded(current_slot);

    if (is_pairing_mode) {
        current_state = BLE_STATE_PAIRING;
    } else {
        current_state = is_bonded ? BLE_STATE_RECONNECTING : BLE_STATE_STANDBY_SILENT;
        btstack_run_loop_remove_timer(&reconnect_timeout_timer);
        if (is_bonded) {
            btstack_run_loop_set_timer_handler(&reconnect_timeout_timer, reconnect_timeout_handler);
            btstack_run_loop_set_timer(&reconnect_timeout_timer, 30000); // 30秒タイムアウト
            btstack_run_loop_add_timer(&reconnect_timeout_timer);
            reconnect_start_us = time_us_64();
        }
    }
    update_ui_for_state(current_state);

    // 【厳格なペアリング制御】未ペアリングかつペアリングモードでない場合は電波を完全停止
    if (!is_pairing_mode && !is_bonded) {
        printf("[BT] Slot %d is unbonded and not in pairing mode. Radio OFF (silent).\r\n", current_slot + 1);
        current_state = BLE_STATE_STANDBY_SILENT;
        update_ui_for_state(current_state);
        return;
    }

    // アドレス設定（Static Random Address）
    bd_addr_t slot_addr;
    get_slot_random_address(current_slot, slot_addr);
    gap_random_address_set_mode(GAP_RANDOM_ADDRESS_TYPE_STATIC);
    gap_random_address_set(slot_addr);

    // アドバタイズデータ設定
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

    // アドバタイズ即時有効化
    adv_set(1);

    printf("[BT] <- apply_slot_identity done (state=%d, pairing=%d, bonded=%d). ADDR=%s\r\n",
           current_state, is_pairing_mode, is_bonded, bd_addr_to_str(slot_addr));
    remote_log_eventf("APPLY-DONE st=%d pair=%d addr=%s", current_state, is_pairing_mode, bd_addr_to_str(slot_addr));
}

// ホットキーから呼ばれる関数
// 同一スロット押下でスロットのON/OFF（電波OFF・切断 / 再接続）をトグルする
void ble_switch_slot(int slot) {
    if (slot < 0 || slot >= MAX_SLOTS) return;

    // 切替・ON/OFFトグル直後の連続操作を禁止（BT 状態の不整合防止）。
    // 受け付けた時点から SWITCH_LOCK_US の間は切替系ホットキーを無視する
    uint64_t now_us = time_us_64();
    if (switch_lock_until_us != 0 && now_us < switch_lock_until_us) {
        printf("[BT] Slot switch locked (%llu ms left)\r\n",
               (unsigned long long)((switch_lock_until_us - now_us) / 1000));
        oled_show_toast("Can't switch now", 1500);
        remote_log_eventf("SWITCH-LOCKED ignored slot=%d", slot + 1);
        return;
    }
    switch_lock_until_us = now_us + SWITCH_LOCK_US;

    ble_lock();
    ble_switch_slot_locked(slot);
    ble_unlock();
}

static void ble_switch_slot_locked(int slot) {
    btstack_run_loop_remove_timer(&bl17_cooldown_timer);
    bl17_reject_in_progress = false;

    if (slot == current_slot) {
        // 同一スロットのトグル操作
        if (con_handle != HCI_CON_HANDLE_INVALID) {
            printf("[BT] Slot %d connected -> Disconnecting to turn Radio OFF\r\n", slot + 1);
            remote_log_eventf("TOGGLE-OFF req (connected) slot=%d", slot + 1);
            pending_slot = slot;
            pending_is_pairing = false;
            pending_toggle_off = true;
            slot_change_requested = true;
            current_state = BLE_STATE_SWITCHING;
            update_ui_for_state(current_state);
            return;
        } else if (current_state == BLE_STATE_RECONNECTING || current_state == BLE_STATE_PAIRING) {
            // 【BL-32】ペアリングモード開始直後（1.5秒以内）は同一スロットのRadio OFFトグルを無視（Alt抜け誤爆防止）
            if (current_state == BLE_STATE_PAIRING && (time_us_64() - pairing_start_us < 1500000)) {
                printf("[BT] Ignored same-slot toggle OFF during pairing grace period (1.5s)\r\n");
                return;
            }
            printf("[BT] Slot %d active advertising -> Turning Radio OFF\r\n", slot + 1);
            remote_log_eventf("RADIO-OFF req (adv) slot=%d", slot + 1);
            is_pairing_mode = false;
            adv_set(0);
            btstack_run_loop_remove_timer(&reconnect_timeout_timer);
            btstack_run_loop_remove_timer(&pairing_timeout_timer);
            current_state = BLE_STATE_STANDBY_SILENT;
            update_ui_for_state(current_state);
            oled_set_radio_off(true);
            oled_show_toast("BT Radio: OFF", 3000);
            schedule_save_radio_off(true); // BL-01: 2秒安定後にFlash保存
            return;
        } else {
            // 電波OFF（SILENT）状態から同一スロットを押下：ボンド情報があれば再接続（ON）
            if (is_slot_bonded(slot)) {
                printf("[BT] Slot %d is OFF -> Turning Radio ON to reconnect\r\n", slot + 1);
                remote_log_eventf("RADIO-ON req slot=%d", slot + 1);
                oled_set_radio_off(false);
                oled_show_toast("BT Radio: ON", 3000);
                schedule_save_radio_off(false); // BL-01: 2秒安定後にFlash保存
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
    remote_log_eventf("SWITCH-REQ cur=%d -> %d", current_slot + 1, slot + 1);
    char sw_toast[32];
    snprintf(sw_toast, sizeof(sw_toast), "Switched to Slot %d", slot + 1);
    oled_show_toast(sw_toast, 2500);
    oled_set_radio_off(false);
    schedule_save_radio_off(false); // BL-01: 新スロット切替時はRadio ON
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
    // ペアリングはカレントスロットのみ対象（別スロット指定ではペアリングを開始しない）
    if (slot < 0 || slot >= MAX_SLOTS || slot != current_slot) return;
    ble_lock();
    ble_enter_pairing_mode_locked(slot);
    ble_unlock();
}

static void ble_enter_pairing_mode_locked(int slot) {
    schedule_save_radio_off(false); // BL-01: ペアリング開始時はRadio ON
    slot_os[slot] = '-';    // 新規ペアリング時はOS表示をリセット
    oled_set_slot_os(slot, '-');
    schedule_save_slot_os();
    is_pairing_mode = true;
    pairing_start_us = time_us_64();
    oled_set_radio_off(false);
    char p_toast[32];
    snprintf(p_toast, sizeof(p_toast), "Pairing Slot %d...", slot + 1);
    oled_show_toast(p_toast, 3000);
    btstack_run_loop_remove_timer(&slot_transition_timer);
    btstack_run_loop_remove_timer(&bl17_cooldown_timer);
    bl17_reject_in_progress = false;
    printf("[BT] Request Pairing Mode for Slot %d\r\n", slot + 1);
    remote_log_eventf("PAIRING-START slot=%d", slot + 1);

    // 【スロットMACアドレスのローテーション（ソルト加算）】
    // ペアリングモード突入時にスロットのMACアドレスを世代更新し、旧ホストの自動接続を遮断
    slot_salts[slot]++;
    save_slot_salts();

    // 【スロット指定のボンド消去】
    // 該当スロットの登録デバイス (slot_peers[slot].addr) のみを le_device_db から検索して物理削除
    if (slot_peers[slot].valid) {
        int max_entries = le_device_db_max_count();
        for (int i = 0; i < max_entries; i++) {
            int a_type = BD_ADDR_TYPE_UNKNOWN;
            bd_addr_t a_addr;
            le_device_db_info(i, &a_type, a_addr, NULL);
            if (a_type != BD_ADDR_TYPE_UNKNOWN && memcmp(a_addr, slot_peers[slot].addr, 6) == 0) {
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
            remote_log_eventf("STACK st=%d", state);
            if (state == HCI_STATE_WORKING) {
                // チップが完全に起動したこの瞬間に初期スロットを適用する
                if (radio_off_saved) {
                    printf("[BT] Boot with Radio OFF saved -> STANDBY_SILENT\r\n");
                    current_state = BLE_STATE_STANDBY_SILENT;
                    adv_set(0);
                    update_ui_for_state(current_state);
                    oled_set_status("Ready");
                    ble_led(0);
                } else {
                    apply_slot_identity(current_slot);
                }
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

                    // 【BL-15】レポートIDごとの購読状態を厳密に個別管理（重複購読時の冗長ログを抑止）
                    bool changed = false;
                    if (report_id == 2) { // Keyboard (Report ID 2)
                        if (ble_kbd_ready != enabled) { ble_kbd_ready = enabled; changed = true; }
                    } else if (report_id == 1) { // Mouse (Report ID 1)
                        if (ble_mouse_ready != enabled) { ble_mouse_ready = enabled; changed = true; }
                    } else if (report_id == 3) { // Consumer Control (Report ID 3: Media Keys)
                        if (ble_consumer_ready != enabled) { ble_consumer_ready = enabled; changed = true; }
                    }

                    if (changed) {
                        printf("[BT] Input report enabled for ID %d: %d\r\n", report_id, enabled);
                        remote_log_eventf("SUBS id=%d en=%d", report_id, enabled);
                        update_connecting_status();
                    }
                    // 【BL-14/15】暗号化完了 かつ キーボード購読完了で安定化タイマ(100ms)開始後に CONNECTED 遷移
                    check_and_start_connection_stabilize();
                    break;
                }
                case HIDS_SUBEVENT_CAN_SEND_NOW: {
                    consecutive_can_send_timeouts = 0;
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

        // 広告の有効化がコントローラに受理されたかを記録する。
        // 0x0C (Command Disallowed) 等が返ると「ペアリングモードなのに発見されない」
        // 状態になるため、status を残す (BL-45 切り分け用)
        case HCI_EVENT_COMMAND_COMPLETE: {
            uint16_t opcode = hci_event_command_complete_get_command_opcode(packet);
            const uint8_t* params = hci_event_command_complete_get_return_parameters(packet);
            if (opcode == HCI_OPCODE_HCI_LE_SET_ADVERTISE_ENABLE) {
                uint8_t st = params[0];
                adv_enabled_ok = (st == 0) && (adv_enable_req != 0);
                remote_log_eventf("ADV-EN status=0x%02X en=%d slot=%d pair=%d st=%d",
                                  st, adv_enable_req, current_slot + 1, is_pairing_mode,
                                  (int) current_state);
            } else if (opcode == HCI_OPCODE_HCI_LE_SET_RANDOM_ADDRESS) {
                remote_log_eventf("ADV-ADDR status=0x%02X", params[0]);
            } else if (opcode == HCI_OPCODE_HCI_LE_SET_ADVERTISING_PARAMETERS) {
                remote_log_eventf("ADV-PARAM status=0x%02X", params[0]);
            }
            break;
        }

        case HCI_EVENT_LE_META: {
            uint8_t subevent = hci_event_le_meta_get_subevent_code(packet);
            if (subevent == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
                hci_con_handle_t new_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                if (con_handle == HCI_CON_HANDLE_INVALID) {
                    btstack_run_loop_remove_timer(&conn_stabilize_timer);
                    conn_stabilize_active = false;
                    btstack_run_loop_remove_timer(&reconnect_timeout_timer);
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
                    remote_log_eventf("LE-CONN slot=%d peer=%s pair=%d", current_slot + 1, current_peer_addr, is_pairing_mode);

                    if (is_pairing_mode || current_state == BLE_STATE_PAIRING) {
                        // Public / Static アドレス直結時の他スロット重複ガード (BL-17)
                        int reg_slot = -1;
                        for (int s = 0; s < MAX_SLOTS; s++) {
                            if (s != current_slot && slot_peers[s].valid) {
                                if (memcmp(slot_peers[s].addr, peer_addr, 6) == 0) {
                                    reg_slot = s;
                                    break;
                                }
                            }
                        }
                        if (reg_slot >= 0) {
                            if (!bl17_reject_in_progress) {
                                printf("[BT] BL-17 Guard: Direct peer %s already bonded to Slot %d. REJECTING pairing on Slot %d!\r\n",
                                       bd_addr_to_str(peer_addr), reg_slot + 1, current_slot + 1);
                                remote_log_eventf("BL17-REJECT slot=%d reg=%d", current_slot + 1, reg_slot + 1);
                                bl17_reject_in_progress = true;
                                gap_disconnect(con_handle);
                            }
                            break;
                        }
                    }

                    // 【BL-15】物理リンク接続時点ではまだ送信禁止（CONNECTING）
                    ble_kbd_ready = false;
                    ble_mouse_ready = false;
                    ble_consumer_ready = false;
                    ble_encrypted = false;
                    ble_can_send = true;
                    slot_disconnect_after_zero = false;
                    zero_sent_waiting_ack = false;
                    adv_set(0);

                    current_state = BLE_STATE_CONNECTING;
                    update_ui_for_state(current_state);

                    // Central (Mac/Win/iOS) 側からのペアリング/暗号化要求を待機（Security Request 重複衝突を防止）
                }
            } else if (subevent == HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE) {
                uint16_t interval = hci_subevent_le_connection_update_complete_get_conn_interval(packet);
                uint16_t latency = hci_subevent_le_connection_update_complete_get_conn_latency(packet);
                uint16_t timeout = hci_subevent_le_connection_update_complete_get_supervision_timeout(packet);
                ble_conn_interval = interval;
                ble_conn_latency = latency;
                ble_conn_timeout = timeout;
                printf("[BT] Connection parameters updated: interval=%.2fms, latency=%d, timeout=%dms\r\n",
                       interval * 1.25f, latency, timeout * 10);

                // 【BL-04】OS自動判別（接続パラメータ・レイテンシ・タイムアウト）
                if (latency > 0) {
                    // Apple (macOS / iOS) は省電力のため latency=4 等を要求
                    update_detected_os('M');
                } else if (timeout >= 450) {
                    // Android のデフォルト supervision timeout は 5000ms (500)
                    peer_has_android_timeout = true;
                    update_detected_os('A');
                }

                // ホスト側が省電力（スレーブラテンシ > 0）を適用してきた場合、
                // マウス操作の追従性を保つために低遅延パラメータ (latency=0) の再適用を要求
                static uint32_t last_param_reassert_ms = 0;
                uint32_t now = to_ms_since_boot(get_absolute_time());
                if (latency > 0 && (now - last_param_reassert_ms > 10000)) {
                    last_param_reassert_ms = now;
                    printf("[BT] Host set latency=%d. Re-requesting low-latency parameters (latency=0)...\r\n", latency);
                    gap_request_connection_parameter_update(con_handle, 9, 12, 0, 400);
                }
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

        case SM_EVENT_IDENTITY_CREATED: {
            int created_index = sm_event_identity_created_get_index(packet);
            printf("[BT] SM_EVENT_IDENTITY_CREATED index=%d\r\n", created_index);
            last_created_device_index = created_index;
            break;
        }

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
                int target_idx = last_created_device_index;
                if (target_idx < 0 || target_idx >= le_device_db_max_count()) {
                    target_idx = le_device_db_count() - 1;
                }
                int newest_idx = target_idx;
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

                last_created_device_index = -1;
                check_and_start_connection_stabilize();
            }
            break;
        }

        // 既知ホストの再接続検知
        case SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED: {
            hci_con_handle_t handle = sm_event_identity_resolving_succeeded_get_handle(packet);
            int device_index = sm_event_identity_resolving_succeeded_get_index(packet);
            printf("[BT] Identity resolved: handle=0x%04X, device_index=%d\r\n", handle, device_index);
            if (is_pairing_mode || current_state == BLE_STATE_PAIRING) {
                // ペアリングモード中に既知デバイスが解決された場合：
                // ホスト側が過去のボンドを削除して新規ペアリングを試行している。
                // Pico側に古いボンドが残っているとSMが鍵衝突で停止するため、古いボンドを削除する。
                int a_type = BD_ADDR_TYPE_UNKNOWN;
                bd_addr_t a_addr;
                le_device_db_info(device_index, &a_type, a_addr, NULL);

                // 【BL-17 防御ガード】解決された Identity が他スロットに登録済みか確認
                int registered_slot = -1;
                for (int s = 0; s < MAX_SLOTS; s++) {
                    if (s != current_slot && slot_peers[s].valid) {
                        if (memcmp(slot_peers[s].addr, a_addr, 6) == 0) {
                            registered_slot = s;
                            break;
                        }
                    }
                }

                if (registered_slot >= 0) {
                    if (!bl17_reject_in_progress) {
                        printf("[BT] BL-17 Guard: Host %s is already bonded to Slot %d. REJECTING pairing on Slot %d!\r\n",
                               bd_addr_to_str(a_addr), registered_slot + 1, current_slot + 1);
                        bl17_reject_in_progress = true;
                        gap_disconnect(handle);
                    }
                    break;
                }

                // 同一スロットの再ペアリングの場合のみ、古いボンドを消去して新規ペアリングを許可
                if (a_type != BD_ADDR_TYPE_UNKNOWN) {
                    printf("[BT] Re-pairing same host on Slot %d. Removing stale bond index=%d\r\n", current_slot + 1, device_index);
                    gap_delete_bonding((bd_addr_type_t)a_type, a_addr);
                }
                le_device_db_remove(device_index);
            }
            // 既知ホストの接続を0msで強制切断しない（再接続無限ループによるホストBTクラッシュの完全防止）
            break;
        }

        case HCI_EVENT_ENCRYPTION_CHANGE: {
            uint8_t status = hci_event_encryption_change_get_status(packet);
            uint8_t enabled = hci_event_encryption_change_get_encryption_enabled(packet);
            if (status == 0 && enabled) {
                printf("[BT] Encryption enabled\r\n");
                remote_log_eventf("ENCR ok slot=%d", current_slot + 1);
                ble_encrypted = true;
                update_connecting_status();

                // 暗号化成功＝このスロットに有効なボンドが存在することを確定
                if (!(slot_bonded_mask & (1 << current_slot))) {
                    slot_bonded_mask |= (1 << current_slot);
                    oled_set_slot_bonded(current_slot, true);
                    printf("[BT] Confirmed Slot %d bonded via encryption, mask=0x%02X\r\n", current_slot + 1, slot_bonded_mask);
                }

                // 【BL-14/15】暗号化完了 かつ キーボード購読完了で安定化タイマ(100ms)開始後に CONNECTED 遷移
                last_created_device_index = -1;
                check_and_start_connection_stabilize();
            } else if (status != 0) {
                printf("[BT] Encryption failed (status=0x%02X). Disconnecting.\r\n", status);
                remote_log_eventf("ENCR FAIL st=0x%02X slot=%d", status, current_slot + 1);
                ble_encrypted = false;
                if (con_handle != HCI_CON_HANDLE_INVALID) {
                    gap_disconnect(con_handle);
                }
            }
            break;
        }

        case ATT_EVENT_MTU_EXCHANGE_COMPLETE: {
            uint16_t mtu = att_event_mtu_exchange_complete_get_MTU(packet);
            printf("[BT] ATT MTU exchanged: %d\r\n", mtu);
            remote_log_eventf("MTU=%d", mtu);
            if (mtu >= 500) {
                if (peer_has_android_timeout || ble_conn_timeout >= 450) {
                    update_detected_os('A');
                } else {
                    update_detected_os('W');
                }
            } else if (mtu >= 200) {
                update_detected_os('W');
            }
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            ble_conn_latency = 0;
            ble_conn_timeout = 0;
            peer_has_android_timeout = false;
            if (con_handle == HCI_CON_HANDLE_INVALID) {
                // 重複イベント（hids_device と hci_event_callback の多重登録）を無視
                break;
            }
            consecutive_can_send_timeouts = 0;
            btstack_run_loop_remove_timer(&disconnect_timeout_timer);
            btstack_run_loop_remove_timer(&conn_stabilize_timer);
                    conn_stabilize_active = false;
            btstack_run_loop_remove_timer(&conn_handshake_timeout_timer);
            con_handle = HCI_CON_HANDLE_INVALID;
            ble_kbd_ready = false;
            ble_mouse_ready = false;
            ble_consumer_ready = false;
            ble_encrypted = false;
            btstack_run_loop_remove_timer(&can_send_timeout_timer);
            ble_can_send = true;
            slot_disconnect_after_zero = false;
            zero_sent_waiting_ack = false;
            current_peer_addr[0] = '\0';

            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            printf("[BT] Disconnected (reason=0x%02X)\r\n", reason);
            health_disc++;
            health_disc_reason = reason;
            remote_log_eventf("DISC rs=0x%02X sw=%d tg=%d pair=%d slot=%d",
                              reason, switch_pending, pending_toggle_off,
                              is_pairing_mode, current_slot + 1);

            // スロット切り替え待ちがある場合、切断完了後に安全に新スロットを適用して再開
            if (switch_pending) {
                switch_pending = false;
                if (pending_toggle_off) {
                    pending_toggle_off = false;
                    is_pairing_mode = false;
                    adv_set(0);
                    current_state = BLE_STATE_STANDBY_SILENT;
                    update_ui_for_state(current_state);
                    oled_set_radio_off(true);
                    oled_show_toast("BT Radio: OFF", 3000);
                    schedule_save_radio_off(true); // BL-01: 2秒安定後にFlash保存
                    printf("[BT] Slot %d Radio turned OFF (Standby Silent).\r\n", current_slot + 1);
                } else {
                    int slot = pending_slot;
                    bool is_pairing = pending_is_pairing;
                    schedule_slot_transition(slot, is_pairing);
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
                if (bl17_reject_in_progress) {
                    bl17_reject_in_progress = false;
                    adv_set(0);
                    printf("[BT] BL-17 Guard: Pausing pairing adv for 2500ms to stop host retry storm\r\n");
                    btstack_run_loop_remove_timer(&bl17_cooldown_timer);
                    btstack_run_loop_set_timer_handler(&bl17_cooldown_timer, bl17_cooldown_timer_handler);
                    btstack_run_loop_set_timer(&bl17_cooldown_timer, 2500);
                    btstack_run_loop_add_timer(&bl17_cooldown_timer);
                } else if (current_state != BLE_STATE_STANDBY_SILENT) {
                    schedule_slot_transition(current_slot, is_pairing_mode);
                } else {
                    adv_set(0);
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
    hids_device_init_with_storage(0, hid_report_descriptor, sizeof(hid_report_descriptor), 3, hid_reports);
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
    ble_lock();
    ble_hid_task_locked();
    ble_unlock();
}

static void ble_hid_task_locked(void) {
    static uint64_t last_countdown_check_us = 0;
    uint64_t now_us = time_us_64();

    // 広告の張り直し (BL-45): 要求は出したのにコントローラが受理していない、
    // または切断処理などで落ちたままのときに 1 秒間隔で再要求する
    if (con_handle == HCI_CON_HANDLE_INVALID && adv_enable_req != 0 && !adv_enabled_ok &&
        (current_state == BLE_STATE_PAIRING || current_state == BLE_STATE_RECONNECTING) &&
        now_us >= adv_retry_next_us) {
        adv_retry_next_us = now_us + 1000000ULL;
        remote_log_eventf("ADV-RETRY st=%d pair=%d req=%d", (int) current_state,
                          is_pairing_mode, adv_enable_req);
        adv_set(1);
    }
    if (now_us - last_countdown_check_us >= 500000) { // 500ms周期でカウントダウン更新
        last_countdown_check_us = now_us;
        if (current_state == BLE_STATE_PAIRING && pairing_start_us > 0) {
            int rem = 60 - (int)((now_us - pairing_start_us) / 1000000);
            if (rem < 0) rem = 0;
            oled_set_pairing_remaining_sec(rem);
        } else if (current_state == BLE_STATE_RECONNECTING) {
            if (reconnect_start_us == 0) {
                reconnect_start_us = now_us;
            }
            int rem = 30 - (int)((now_us - reconnect_start_us) / 1000000);
            if (rem < 0) rem = 0;
            oled_set_reconnect_remaining_sec(rem);
        } else {
            reconnect_start_us = 0;
        }
    }

    if (slot_change_requested) {
        slot_change_requested = false;
        btstack_run_loop_remove_timer(&bl17_cooldown_timer);
        bl17_reject_in_progress = false;
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
    if (report_id == 2 && !ble_kbd_ready) return;      // Report ID 2: Keyboard
    if (report_id == 1 && !ble_mouse_ready) return;    // Report ID 1: Mouse
    if (report_id == 3 && !ble_consumer_ready) return; // Report ID 3: Consumer (BL-12)
    if (report_id != 1 && report_id != 2 && report_id != 3) return;

    if (!ble_can_send) {
        health_blocked++; // フロー制御待ちで捨てた (ウェッジ時に増える)
        return;
    }

    ble_lock();

    oled_notify_tx();
    uint8_t status = hids_device_send_input_report_for_id(con_handle, report_id, (uint8_t*)data, len);
    if (status == ERROR_CODE_SUCCESS) {
        health_ok++;
        health_last_ok_us = time_us_64();
    } else if (status == 0x57) { // BTSTACK_ACL_BUFFERS_FULL: 受理されず再要求になる
        health_acl_full++;
    }
    if (status == ERROR_CODE_SUCCESS || status == 0x57) { // 0x57: BTSTACK_ACL_BUFFERS_FULL
        ble_can_send = false;
        btstack_run_loop_remove_timer(&can_send_timeout_timer);
        btstack_run_loop_set_timer_handler(&can_send_timeout_timer, can_send_timeout_handler);
        btstack_run_loop_set_timer(&can_send_timeout_timer, 2000); // 2秒の安全ウォッチドッグ
        btstack_run_loop_add_timer(&can_send_timeout_timer);
        uint8_t req_err = hids_device_request_can_send_now_event(con_handle);
        if (req_err != ERROR_CODE_SUCCESS) {
            ble_can_send = true;
            btstack_run_loop_remove_timer(&can_send_timeout_timer);
        }
    } else {
        printf("[BLE-ERR] send_report id=%d len=%d status=0x%02X\r\n", report_id, len, status);
        ble_can_send = true;
    }

    ble_unlock();
}


void ble_hid_kick_reconnect(void) {
    ble_lock();
    ble_hid_kick_reconnect_locked();
    ble_unlock();
}

static void ble_hid_kick_reconnect_locked(void) {
    if (current_state == BLE_STATE_RECONNECTING && con_handle == HCI_CON_HANDLE_INVALID) {
        static uint32_t last_kick_ms = 0;
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_kick_ms > 2000) {
            last_kick_ms = now;
            printf("[BT] Activity detected in RECONNECTING state. Re-asserting advertisement.\r\n");
            adv_set(1);
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

// カレントスロットのボンドだけを消去する（ペアリングは開始しない）。
// 2 段階ペアリングホットキーの 1 回目で使う
static void ble_clear_slot_bonds_locked(int slot) {
    if (slot_peers[slot].valid) {
        int max_entries = le_device_db_max_count();
        for (int i = 0; i < max_entries; i++) {
            int a_type = BD_ADDR_TYPE_UNKNOWN;
            bd_addr_t a_addr;
            le_device_db_info(i, &a_type, a_addr, NULL);
            if (a_type != BD_ADDR_TYPE_UNKNOWN && memcmp(a_addr, slot_peers[slot].addr, 6) == 0) {
                gap_delete_bonding((bd_addr_type_t) a_type, a_addr);
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
    slot_os[slot] = '-';
    oled_set_slot_os(slot, '-');
    schedule_save_slot_os();
    printf("[BT] Slot %d bond cleared\r\n", slot + 1);
    remote_log_eventf("BONDCLR slot=%d", slot + 1);

    if (con_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(con_handle); // 切断完了ハンドラが未ボンドとして待機状態へ遷移する
    } else {
        is_pairing_mode = false;
        current_state = BLE_STATE_STANDBY_SILENT;
        update_ui_for_state(current_state);
        adv_set(0);
    }
    oled_show_toast("Slot Cleared", 2000);
}

static void ble_clear_slot_bonds(int slot) {
    if (slot < 0 || slot >= MAX_SLOTS) return;
    ble_lock();
    ble_clear_slot_bonds_locked(slot);
    ble_unlock();
}

void ble_clear_bonds(void) {
    ble_lock();
    ble_clear_bonds_locked();
    ble_unlock();
}

static void ble_clear_bonds_locked(void) {
    remote_log_eventf("ALLCLEAR bonding wipe");
    btstack_run_loop_remove_timer(&bl17_cooldown_timer);
    bl17_reject_in_progress = false;

    // 以降の切断完了イベントや切替キューが all-clear 前の状態を参照しないように
    // pending / 状態を明示的にクリアする
    slot_change_requested = false;
    pending_slot = -1;
    pending_is_pairing = false;
    pending_toggle_off = false;
    switch_pending = false;
    is_pairing_mode = false;
    pairing_start_us = 0;

    // 伝送/送信準備フラグも無効化して、残滓レポートを抑制する
    ble_kbd_ready = false;
    ble_mouse_ready = false;
    ble_consumer_ready = false;
    ble_encrypted = false;
    ble_can_send = true;
    consecutive_can_send_timeouts = 0;
    slot_disconnect_after_zero = false;
    zero_sent_waiting_ack = false;
    current_peer_addr[0] = '\0';

    // 復帰用タイマ/遷移タイマを停止
    btstack_run_loop_remove_timer(&slot_transition_timer);
    btstack_run_loop_remove_timer(&pairing_timeout_timer);
    btstack_run_loop_remove_timer(&reconnect_timeout_timer);
    btstack_run_loop_remove_timer(&disconnect_timeout_timer);
    btstack_run_loop_remove_timer(&conn_handshake_timeout_timer);
    btstack_run_loop_remove_timer(&conn_stabilize_timer);
    btstack_run_loop_remove_timer(&can_send_timeout_timer);
    if (con_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(con_handle);
    }
    int max_entries = le_device_db_max_count();
    for (int i = 0; i < max_entries; i++) {
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
    printf("[BT] Cleared all bonds from DB and Resolving List (max_entries=%d)\r\n", max_entries);
    for (int i = 0; i < 4; i++) {
        oled_set_slot_bonded(i, false);
        slot_os[i] = '-';
        oled_set_slot_os(i, '-');
    }
    btstack_run_loop_remove_timer(&save_slot_timer);
    pending_save_slot = 0xFF;
    pending_save_os = false;
    pending_save_radio_off = false;
    if (tlv_impl) {
        tlv_impl->store_tag(&tlv_context, TLV_TAG_SLOT_OS, (const uint8_t*)slot_os, sizeof(slot_os));
    }
    current_state = BLE_STATE_STANDBY_SILENT;
    update_ui_for_state(current_state);
    oled_set_status("Bonds Cleared");
    oled_show_toast("All Bonds Cleared!", 3000);
    adv_set(0);
    ble_led(0);
}

void ble_led(bool value) {
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, value);
}



// =========================================================================
// 送信・トランスポートディスパッチおよびスロット切替ホットキー処理
// =========================================================================
#ifndef OR_BUFSIZE
#define OR_BUFSIZE 8
#endif
#ifndef MAX_REPORT_SIZE
#define MAX_REPORT_SIZE 64
#endif
extern uint8_t outgoing_reports[OR_BUFSIZE][MAX_REPORT_SIZE + 1];
extern uint8_t or_head;
extern uint8_t or_items;

static uint64_t last_mouse_pace_us = 0;
static uint8_t last_mouse_buttons = 0;

extern bool send_report(ble_send_report_fn_t do_send_report);

static void send_all_release_reports(void) {
    // USB 側にキーボード全離下を送信
    usb_clear_kbd_report();

    // BLE 側にもキーボード全離下を送信
    if (ble_hid_is_connected()) {
        uint8_t zero_kbd[16] = {0};
        ble_hid_send_report(2, zero_kbd, sizeof(zero_kbd));
    }
}

// キーボードのホットキー（Win/Cmd + Ctrl + F1..F4）をチェックする関数
// ホットキーが押された場合は true を返し、OS へレポートを送信しない（消費する）
bool ble_hid_check_hotkey(uint8_t interface, const uint8_t* report_with_id, uint8_t len) {
    (void)interface;
    if (len < 9) return false;

    uint8_t report_id = report_with_id[0];
    const uint8_t* report_data = report_with_id + 1;

    // キーボードレポート（ID 2）のみ対象
    if (report_id != 2) return false;

    uint8_t modifier = report_data[0];

    // Left/Right の Ctrl (0x01 / 0x10) と Win/GUI (0x08 / 0x80) の両方が含まれているか
    bool ctrl_pressed = (modifier & 0x11) != 0; // Left/Right Ctrl
    bool gui_pressed  = (modifier & 0x88) != 0; // Left/Right GUI (Win/Cmd)
    bool alt_pressed  = (modifier & 0x44) != 0; // Left/Right Alt

    // F1 〜 F4 のスロット判定（通常数字キーやテンキーの誤判定を完全に排除）
    int slot = -1;
    // Backspace (Usage 0x2A: report_data[5] bit 6 / 0x40) または Forward Delete (Usage 0x4C: report_data[10] bit 0 / 0x01) でボンド全消去
    bool backspace_pressed = ((report_data[5] & 0x40) != 0) || ((report_data[10] & 0x01) != 0);
    if      ((report_data[7] & 0x40) != 0) slot = 0; // F1 -> Slot 1
    else if ((report_data[7] & 0x80) != 0) slot = 1; // F2 -> Slot 2
    else if ((report_data[8] & 0x01) != 0) slot = 2; // F3 -> Slot 3
    else if ((report_data[8] & 0x02) != 0) slot = 3; // F4 -> Slot 4

    static int last_pressed_slot = -1;
    static bool last_alt_pressed = false;
    static bool clear_bonds_was_pressed = false;

    // 全ボンド消去ホットキー: Ctrl + Win + Alt + Backspace / Delete
    if (ctrl_pressed && gui_pressed && alt_pressed && backspace_pressed) {
        if (!clear_bonds_was_pressed) {
            clear_bonds_was_pressed = true;
            send_all_release_reports();
            printf("[BT] *** HOTKEY: Clear All BLE Bonds ***\r\n");
            ble_clear_bonds();
        }
        return true;
    }
    clear_bonds_was_pressed = false;

    // ペアリングホットキー（2段階・カレントスロット専用）: Ctrl + Win + Alt + Space
    // 1回目: カレントスロットのボンドを消去 / 2回目 (消去済み): そのスロットでペアリング開始
    bool space_pressed = (report_data[6] & 0x01) != 0; // Usage 0x2C (Space)
    static bool space_was_pressed = false;
    if (ctrl_pressed && gui_pressed && alt_pressed && space_pressed) {
        if (!space_was_pressed) {
            space_was_pressed = true;
            send_all_release_reports();
            if (is_slot_bonded(current_slot)) {
                printf("[BT] *** HOTKEY: Clear bond of current slot %d ***\r\n", current_slot + 1);
                ble_clear_slot_bonds(current_slot);
            } else {
                printf("[BT] *** HOTKEY: Start pairing for current slot %d ***\r\n", current_slot + 1);
                ble_enter_pairing_mode(current_slot);
            }
        }
        return true;
    }
    space_was_pressed = false;

    // OS手動切替ホットキー: Ctrl + Win + Alt + 'O'
    // 'O' は Usage 0x12 -> report_data[2] bit 6 (0x40)
    bool o_pressed = (report_data[2] & 0x40) != 0;
    static bool o_was_pressed = false;
    if (ctrl_pressed && gui_pressed && alt_pressed && o_pressed) {
        if (!o_was_pressed) {
            o_was_pressed = true;
            send_all_release_reports();
            char cur = slot_os[current_slot];
            char next_os = 'M';
            if (cur == 'M') next_os = 'W';
            else if (cur == 'W') next_os = 'A';
            else if (cur == 'A') next_os = 'I';
            else if (cur == 'I') next_os = '-';
            else next_os = 'M';

            slot_os[current_slot] = next_os;
            oled_set_slot_os(current_slot, next_os);
            schedule_save_slot_os(); // 2秒安定後にFlashへ遅延保存（連打時のFlash摩耗・競合防止）
            char os_toast[32];
            snprintf(os_toast, sizeof(os_toast), "Slot %d OS -> %c", current_slot + 1, next_os);
            oled_show_toast(os_toast, 3000);
            printf("[BT] *** HOTKEY: Slot %d OS set to %c ***\r\n", current_slot + 1, next_os);
        }
        return true;
    }
    o_was_pressed = false;

    // Alt 付きの F キーはスロット操作に使わない（OS 側のショートカットへそのまま通す）
    int current_slot_request = (ctrl_pressed && gui_pressed && !alt_pressed && slot >= 0) ? slot : -1;

    if (current_slot_request >= 0) {
        if (current_slot_request != last_pressed_slot || alt_pressed != last_alt_pressed) {
            last_pressed_slot = current_slot_request;
            last_alt_pressed = alt_pressed;
            send_all_release_reports();
            printf("[BT] *** HOTKEY: Switch to slot %d ***\r\n", slot + 1);
            ble_switch_slot(slot);
        }
        return true; // ホットキー成立中は常に消費
    }

    // Fキーまたは修飾キーが離されたらリセット
    last_pressed_slot = -1;
    last_alt_pressed = false;
    return false;
}

// 未接続（給電のみ）専用関数：スロット切替ホットキーの判定とキュー消化
static bool do_send_report_offline(uint8_t interface, const uint8_t* report_with_id, uint8_t len) {
    ble_hid_check_hotkey(interface, report_with_id, len);
    ble_hid_kick_reconnect();
    return true;
}

static bool do_send_report_ble(uint8_t interface, const uint8_t* report_with_id, uint8_t len) {
    if (ble_hid_check_hotkey(interface, report_with_id, len)) {
        return true;
    }
    uint8_t report_id = report_with_id[0];
    const uint8_t* report_data = report_with_id + 1;
    uint8_t report_len = len - 1;

#if ENABLE_OLED
    oled_set_mode(true); // BLE mode
    oled_notify_tx();
#endif

    if (report_id == 1 && report_len >= 5) {
        last_mouse_pace_us = time_us_64();
        last_mouse_buttons = report_data[0];
    }

    ble_hid_send_report(report_id, report_data, report_len);
    return true;
}

void ble_transport_task(ble_send_report_fn_t do_send_usb) {
    static bool prev_ble_connected = false;
    bool curr_ble_connected = ble_hid_is_connected();
    if (curr_ble_connected != prev_ble_connected) {
        prev_ble_connected = curr_ble_connected;
        printf("[BT] Active transport changed: %s\r\n", curr_ble_connected ? "BLE" : "USB");
        if (curr_ble_connected) {
            printf("[BT] Switched to BLE priority, clearing USB reports\r\n");
            usb_clear_kbd_report();
        }
        last_mouse_pace_us = 0;
        last_mouse_buttons = 0;
    }

    if (curr_ble_connected) {
        // =========================================================
        // BLE 伝送パス（無線インターバル完全同期）
        // =========================================================
        bool ready_to_send = ble_hid_can_send();
        if (ready_to_send && or_items == 1) {
            uint8_t next_id = outgoing_reports[or_head][0];
            if (next_id == 1) { // Mouse report
                uint8_t cur_buttons = outgoing_reports[or_head][1];
                uint64_t now_us = time_us_64();
                if (cur_buttons == last_mouse_buttons) {
                    uint16_t conn_interval = ble_hid_get_conn_interval();
                    uint32_t interval_us = (conn_interval > 0) ? (conn_interval * 1250) : 15000;
                    // 1接続インターバルあたり厳密に最大1パケットへ整合（66.6pkts/s上限）
                    // 14.5ms以上の間隔を保証し、1つの15msアンカー内に2パケット押し込まれるバースト衝突を完全に防止
                    uint32_t pace_us = (interval_us > 500) ? (interval_us - 500) : 14500;
                    if (now_us - last_mouse_pace_us < pace_us) {
                        ready_to_send = false;
                    }
                }
            }
        }
        if (ready_to_send) {
            send_report(do_send_report_ble);
        }
    } else if (usb_is_mounted()) {
        // =========================================================
        // USB 伝送パス（有線 Remapper と 100% 同一の純粋な TinyUSB 処理）
        // =========================================================
        if (usb_is_ready()) {
            send_report(do_send_usb);
        }
    } else {
        // =========================================================
        // 未接続パス（給電のみ）：キュー溢れ防止とホットキー検出
        // =========================================================
        send_report(do_send_report_offline);
    }
    ble_hid_task();
}

#endif // ENABLE_BLE
