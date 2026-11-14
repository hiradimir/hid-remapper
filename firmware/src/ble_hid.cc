#if ENABLE_BLE

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/async_context.h"
#include "pico/unique_id.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"  // IRQ 停止ストレスの計装用

// 【2026-11-14】送信キュー (再現テストで積むため、定義を前方に移動)
#ifndef OR_BUFSIZE
#define OR_BUFSIZE 8
#endif
#ifndef MAX_REPORT_SIZE
#define MAX_REPORT_SIZE 64
#endif
extern uint8_t outgoing_reports[OR_BUFSIZE][MAX_REPORT_SIZE + 1];
extern uint8_t or_head;
extern uint8_t or_items;
extern uint8_t or_tail; // 【2026-11-14】再現テストでキューに積むため

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
// BLE_LOCK_DISABLE=1 にすると no-op になる。BL-45 (ペアリング検出不可) の
// 切り分け専用スイッチで、常用ビルドでは 0 のまま (BL-33 の排他を外さない)
#if defined(BLE_LOCK_DISABLE) && BLE_LOCK_DISABLE
static inline void ble_lock(void) {}
static inline void ble_unlock(void) {}
#else
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
#endif

static int consecutive_can_send_timeouts = 0;

// 広告の有効化は「コマンドは投げたがコントローラに拒否された (0x0C 等)」場合に
// 無言で失敗し、ペアリングモードなのに誰からも見えない状態になる (BL-45)。
// 要求値と受理結果を保持し、未達なら ble_hid_task 側で張り直す
// ボンド済み IRK をコントローラのリゾルビングリストへ読み込めたか。
// これが無いと RPA を使うホストを accept list で照合できない
static bool resolving_list_loaded = false;

// 0x57 (ACL バッファ満杯) が続いて通知が成功しない状態からの復旧用。
// 再接続では直らず、コントローラの再起動だけが効く (2026-10-23 01:48 の実測)
static volatile bool ctrl_reset_pending = false;
// 膠着復旧時に BT コアのファームを再投入するか (2026-10-31)
static volatile bool ctrl_reload_btcore = false;
// 接続中だが 0x57 が連発して通知が成功しない状態 (トランスポートとしては死んでいる)。
// USB フォールバックの判断に使う
static volatile bool health_tx_stalled = false;
// 連続 0x57 の回数 (送信成功で 0 に戻す)。タイマー再アームに依存せず膠着を数える
static volatile uint16_t health_acl_run = 0;
// 最後に「長時間の無送信から復帰した」時刻。復帰直後はホスト側が不安定なので
// 接続パラメータの再要求を 60 秒控える (2026-11-01 13:20 の連続切断への配慮)
static volatile uint64_t last_resume_us = 0;
// 【2026-11-03】送信スロット待ちが高い状態は膠着の前兆 (15:26 の膠着では
// 6 分前から 36〜50ms が続いていた)。30ms 以上が 60 秒続いたら、チップが
// 固まる前に自分から切断して張り直す。0 にすると無効化できる
#ifndef BLE_LAT_HIGH_MS
#define BLE_LAT_HIGH_MS 30
#endif
#ifndef BLE_LAT_HIGH_WINDOWS
#define BLE_LAT_HIGH_WINDOWS 6   // 10 秒窓 × 6 = 60 秒
#endif
#ifndef BLE_LAT_WATCH_ENABLE
#define BLE_LAT_WATCH_ENABLE 1
#endif
// 「クレジットが返らない」と判断して送信を絞っている状態 (1 秒に 1 回だけ試す)
static volatile bool credit_stall = false;
static uint64_t credit_stall_next_probe_us = 0;
// Connection Update Complete の受信回数
static volatile uint32_t conn_update_count = 0;
// 広告の有効化を遅延させる。disable と enable を同じティックで呼ぶと
// BTstack が「変化なし」と判定してコマンドを送らないため、時間を離す
// 再接続タイムアウトで電波を落とした状態 (ユーザー操作の OFF とは区別する)。
// 入力イベントを契機に、もう一度だけ接続を試みる
static volatile bool dormant_retry = false;
static uint64_t dormant_last_try_us = 0;

static volatile bool adv_enable_pending = false;
static uint64_t adv_enable_at_us = 0;

// 【2026-11-12】広告が出たかどうかの監視。
// チップが固まると LE Set Advertise Enable の完了通知が返らなくなり、
// 広告が出ないまま「切断中」で居座る (2026-10-31 / 11-09 / 11-12 の共通症状)。
// ADV-EN が返らなければコントローラが HCI を受け付けていない = 復旧不能と判断する
#ifndef BLE_DEAD_DETECT_ENABLE
#define BLE_DEAD_DETECT_ENABLE 1
#endif
#ifndef BLE_DEAD_ADV_TIMEOUT_MS
#define BLE_DEAD_ADV_TIMEOUT_MS 3000
#endif
#ifndef BLE_DEAD_ADV_RETRIES
#define BLE_DEAD_ADV_RETRIES 3
#endif
// 自動再起動を許す最低稼働時間。起動直後の連鎖再起動を防ぐ
#ifndef BLE_AUTO_REBOOT_MIN_UPTIME_S
#define BLE_AUTO_REBOOT_MIN_UPTIME_S 3600
#endif
static volatile uint64_t adv_pending_since_us = 0;
// 【2026-11-13】PROACT で切断要求を出した時刻。切断完了イベントも広告も来ない
// (= コントローラが HCI を受け付けない) ケースを、他の要因に邪魔されず検出する
#ifndef BLE_PROACT_DEAD_MS
#define BLE_PROACT_DEAD_MS 15000
#endif
static volatile uint64_t proact_pending_us = 0;
// 【2026-11-13】HCI コマンドクレジット枯渇 (ccmd=0) 対策。
// hci_power_control(ON) は num_cmd_packets を 1 に戻すので、電源を入れ直せば
// クレジットが復活する。再起動の前に 1 回だけ試す
static bool hci_recycle_done = false;
static uint8_t adv_fail_count = 0;
static bool ble_dead_declared = false;
static bool ble_dead_snapshot_done = false;
static uint64_t ble_dead_since_us = 0;
static uint64_t ble_reboot_at_us = 0;
static uint64_t ble_dead_wait_log_us = 0;

static volatile int adv_enable_req = 0;
static volatile bool adv_enabled_ok = false;
// コントローラが広告有効化を**拒否した**ときだけ立てる (成功時の取りこぼしで
// 再要求すると、アドレス/パラメータ設定の途中で広告が立ち、以後の設定が
// 反映されないレースになる)
static volatile bool adv_retry_pending = false;
// 【2026-10-31 00:44 実測】BT コア再投入で STACK は WORKING (st=2) まで戻るのに、
// 広告有効化 (LE_SET_ADVERTISE_ENABLE) の完了通知が返らず広告が出ないまま
// 「復旧した」と判断して休眠に入った。再投入後は広告が本当に出たか確認する
static bool post_reload = false;
static uint64_t adv_verify_deadline_us = 0;
static uint8_t adv_verify_count = 0;
static uint64_t adv_retry_next_us = 0;

#ifndef CMD_COLLIDE_US
#define CMD_COLLIDE_US 0
#endif
// 【2026-11-14】再現テスト (検証専用): 一定周期で「切断 → 再広告」を自動で繰り返し、
// 接続/切断のたびに HCI コマンドが連続して送られる状況を作る。
// 膠着が「応答の取りこぼし」なら、この回転を速くすると再現しやすい。0 で無効
#ifndef REPRO_CYCLE_MS
#define REPRO_CYCLE_MS 0
#endif
// 【2026-11-14】再現テスト (検証専用): 一定周期でゼロ移動のマウスレポートを送り続け、
// 「ずっと使っている」状態を合成する。カーソルは動かないが ACL 送信は発生する。
// 膠着が「送信し続けている最中」に起きるなら、これで狙える。0 で無効
#ifndef REPRO_FLOOD_MS
#define REPRO_FLOOD_MS 0
#endif

static void adv_set(int enable) {
    adv_enable_req = enable;
    // 【2026-11-12】enable 要求から ADV-EN 完了までの経過を測る
    if (enable) {
        adv_pending_since_us = time_us_64();
    } else {
        adv_pending_since_us = 0;
    }
    gap_advertisements_enable(enable);
#if CMD_COLLIDE_US > 0
    // 【2026-11-13】衝突テスト (検証専用): コマンド送信直後に IRQ を止めて、
    // 「応答が返る瞬間にホストが止まる」状況を確実に作る。
    // ここでクレジットが落ちれば、膠着の機序がこれで確定する
    {
        uint32_t irq_state = save_and_disable_interrupts();
        uint64_t t0 = time_us_64();
        while ((time_us_64() - t0) < CMD_COLLIDE_US) {
            tight_loop_contents();
        }
        restore_interrupts(irq_state);
        remote_log_eventf("BT CMD-COLLIDE us=%u en=%d", (unsigned) CMD_COLLIDE_US, enable);
    }
#endif
}


static int current_slot = 0;

uint32_t ble_hid_get_connected_slot_mask(void) {
    return (current_state == BLE_STATE_CONNECTED) ? (1u << current_slot) : 0;
}

// BL-43/45 ウェッジ観測用カウンタ。リアルタイム経路では加算のみ行う
static volatile uint32_t health_ok = 0;
static volatile uint32_t health_acl_full = 0;
static volatile uint32_t health_blocked = 0;
static volatile uint32_t health_skip_state = 0;  // 接続状態でなくて捨てた
static volatile uint32_t health_skip_enc = 0;    // 未暗号化で捨てた
static volatile uint32_t health_skip_ready = 0;  // 未購読で捨てた
static volatile uint32_t health_err = 0;
static volatile uint32_t health_burst = 0;
// 送信→完了レイテンシ: 受理時刻をリングに積み、Number Of Completed Packets で
// 取り出して遅延を測る。BTstack より下 (コントローラ/エアタイム) はこれでしか見えない
// 送信レイテンシ: 「CAN_SEND_NOW を要求した時刻 → 許可イベントが来た時刻」。
// 完了パケットとの突き合わせは ATT 応答など入力以外の ACL が混ざって値が膨らむため、
// BTstack のスケジューリング待ちそのものを測る (2026-10-27 変更)
static volatile uint64_t can_send_req_us = 0;
static volatile uint32_t health_lat_max_ms = 0;
// 【2026-11-03】膠着の前兆検知用。remote_log は health_lat_max_ms を HB ごとに
// リセットするため、こちらで 10 秒窓の最大値を別に持つ
static volatile uint32_t health_lat_win_max_ms = 0;

static volatile uint32_t health_dmax = 0;
void ble_hid_reset_lat_max(void) { health_lat_max_ms = 0; }
void ble_hid_reset_dmax(void) { health_dmax = 0; }
// コントローラが返した "Number Of Completed Packets" の回数と総数。
// 0x57 膠着時にこれが止まっていれば「コントローラがクレジットを返していない」
static volatile uint32_t health_cp_events = 0;
// 【2026-11-07】膠着の切り分け用。最後に HCI イベントを受けた時刻と、最後に
// Completed Packets を受けた時刻。CANSEND が来ない瞬間のこの 2 つで
// 「チップが止まった (hci_idle 大)」か「電波/相手が応答しない (cp_idle だけ大)」を分ける
static volatile uint64_t health_last_hci_us = 0;
static volatile uint64_t health_last_cp_us = 0;
// 【2026-11-12】起動後の接続成立回数 (寿命仮説①の判定用)
static volatile uint32_t health_conn_count = 0;
static volatile uint32_t health_cp_packets = 0;   // 0x57 以外の送信エラー (ok が伸びない原因の切り分け)
static volatile uint32_t health_disc = 0;
static volatile uint8_t health_disc_reason = 0;
static volatile uint64_t health_last_ok_us = 0;

void ble_hid_get_health(ble_hid_health_t* out) {
    if (out == nullptr) return;
    out->ci = ble_hid_get_conn_interval();
    out->ok = health_ok;
    out->acl_full = health_acl_full;
    out->blocked = health_blocked;
    out->err = health_err;
    out->cp_events = health_cp_events;
    out->cp_packets = health_cp_packets;
    out->burst = health_burst;
    out->clat = ble_conn_latency;
    out->st = ble_conn_timeout;
    out->cu = conn_update_count;
    out->acl_run = health_acl_run;
    out->conn_count = health_conn_count;
    out->lat_max_ms = health_lat_max_ms;
    out->dmax = health_dmax;
    out->disc = health_disc;
    out->disc_reason = health_disc_reason;
    out->last_ok_us = health_last_ok_us;
    out->sk_state = health_skip_state;
    out->sk_enc = health_skip_enc;
    out->sk_ready = health_skip_ready;
    out->flags = (uint8_t) ((current_state == BLE_STATE_CONNECTED ? 0x01 : 0) |
                            (ble_encrypted ? 0x02 : 0) |
                            (ble_mouse_ready ? 0x04 : 0) |
                            (ble_kbd_ready ? 0x08 : 0) |
                            (ble_consumer_ready ? 0x10 : 0) |
                            (health_tx_stalled ? 0x20 : 0) |
                            (dormant_retry ? 0x40 : 0));
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
        {
            // 膠着の切り分け: チップは生きているか / Wi-Fi 送信と重なったか
            uint64_t now_d = time_us_64();
            uint32_t hci_idle = (health_last_hci_us != 0)
                                    ? (uint32_t) ((now_d - health_last_hci_us) / 1000ULL)
                                    : 0xFFFFFFFFu;
            uint32_t cp_idle = (health_last_cp_us != 0)
                                   ? (uint32_t) ((now_d - health_last_cp_us) / 1000ULL)
                                   : 0xFFFFFFFFu;
            remote_log_eventf("CANSEND-TO n=%d hci_idle=%ums cp_idle=%ums rlog_age=%ums rssi=%d conn=%u heap=%u temp=%d ccmd=%d",
                              consecutive_can_send_timeouts, (unsigned) hci_idle,
                              (unsigned) cp_idle, (unsigned) remote_log_tx_age_ms(),
                              remote_log_last_rssi(), (unsigned) health_conn_count,
                              (unsigned) remote_log_heap_used(), remote_log_chip_temp_c(),
                              hci_can_send_command_packet_now() ? 1 : 0);
        }
        // 【2026-11-09】送信要求が 2 秒通らない = アプリ状態が CONNECTED でも
        // リンクは実質死んでいる。ここで tx_stalled を立てないと、転送先が
        // BLE のまま固定されて USB フォールバックが動かず、OLED も
        // 「繋がっている」表示のままになる (2026-11-09 23:02 実測)。
        // 送信が 1 回でも成功すれば health_ok が進み、自動で解除される
        health_tx_stalled = true;

        // 【2026-11-06】2 秒間 CAN_SEND_NOW が来ない = リンクが詰まり始めた最初の合図。
        // 09:19 も 01:38 も 12:31 も、この「2 秒待ち」が最初の症状で、そこから
        // 数十秒でクレジットが凍結し、チップが固まって再投入でも戻らなくなった。
        // 固まる前に自分から切断 → 再広告すれば、0.5 秒程度で張り直せる見込み。
        // 誤発動の連鎖を防ぐため 30 秒に 1 回まで
#ifndef BLE_CANSEND_PROACT_ENABLE
#define BLE_CANSEND_PROACT_ENABLE 1
#endif
#if BLE_CANSEND_PROACT_ENABLE
        if (consecutive_can_send_timeouts == 1) {
            static uint64_t last_proact_us = 0;
            uint64_t now_p = time_us_64();
            if ((now_p - last_proact_us) >= 30000000ULL) {
                last_proact_us = now_p;
                remote_log_eventf("BT CANSEND-PROACT 2s no grant -> disconnect");
                // 【2026-11-13】切断要求を出した = 以降は広告が立ち上がるはず。
                // 切断完了イベントが来ない場合はここから 3 秒で検出する
                adv_pending_since_us = time_us_64();
                proact_pending_us = time_us_64();
                gap_disconnect(con_handle);
#if CMD_COLLIDE_US > 0
                // 【2026-11-13】衝突テスト: 切断コマンドの応答中に IRQ を止める
                // （実機で応答を落としたのがこのコマンドだったため）
                {
                    uint32_t irq_state = save_and_disable_interrupts();
                    uint64_t t0 = time_us_64();
                    while ((time_us_64() - t0) < CMD_COLLIDE_US) {
                        tight_loop_contents();
                    }
                    restore_interrupts(irq_state);
                    remote_log_eventf("BT CMD-COLLIDE us=%u en=disc",
                                      (unsigned) CMD_COLLIDE_US);
                }
#endif
                return;
            }
        }
#endif

        // 【2026-10-31 変更】以前は 2 回連続 (4 秒) で gap_disconnect していたが、
        // 膠着時は切断コマンド自体がチップに届かず、生きているリンクを壊すだけだった。
        // ホストのスリープ中も同じ症状になるため、切断はしない。
        // 3 回目 (6 秒) で「クレジットが返らない」と判断し、送信を 1 秒 1 回の
        // プローブに絞る (0x57 の洪水を止めてチップとホストに休ませる)
        if (consecutive_can_send_timeouts >= 3 && !credit_stall) {
            credit_stall = true;
            credit_stall_next_probe_us = time_us_64() + 1000000ULL;
            remote_log_eventf("BT CREDIT-STALL start aclrun=%u acl=%u",
                              (unsigned) health_acl_run, (unsigned) health_acl_full);
        }

        ble_can_send = true;
        hids_device_request_can_send_now_event(con_handle);
    }
}

static void reconnect_timeout_handler(btstack_timer_source_t *ts) {
    if (con_handle == HCI_CON_HANDLE_INVALID && current_state == BLE_STATE_RECONNECTING) {
        printf("[BT] Reconnection timeout (30s expired). Radio OFF.\r\n");
        remote_log_eventf("RECONN-TO radio-off (dormant; retry on input)");
        adv_set(0);
        dormant_retry = true;
        oled_set_radio_off(true); // 「Connecting...(Ns)」のまま固まらないようにする // 入力があれば接続を再試行する
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
        dormant_retry = false;
        // 再接続時にホスト側の押下状態が残っていることがある (マウス左ボタン固着等)。
        // 接続直後に全離下を 1 回送って状態を揃える
        {
            uint8_t z_kbd[16] = {0};
            uint8_t z_mouse[9] = {0};
            uint8_t z_consumer[1] = {0};
            if (ble_kbd_ready) {
                hids_device_send_input_report_for_id(con_handle, 2, z_kbd, sizeof(z_kbd));
            }
            if (ble_mouse_ready) {
                hids_device_send_input_report_for_id(con_handle, 1, z_mouse, sizeof(z_mouse));
            }
            if (ble_consumer_ready) {
                hids_device_send_input_report_for_id(con_handle, 3, z_consumer, sizeof(z_consumer));
            }
            remote_log_eventf("BT RELEASE-ALL on connect");
        }
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

// 【2026-11-13】TLV (Flash) 書き込みの所要時間を記録する。
// Flash 書き込み中は XIP が止まるため、CYW43 ドライバが動けず HCI イベントを
// 取りこぼす仮説の検証用。1ms 以上かかったときだけ記録する
static void tlv_store_timed(uint32_t tag, const void* data, int len, const char* kind) {
    if (!tlv_impl) return;
    uint64_t t0 = time_us_64();
    tlv_impl->store_tag(&tlv_context, tag, (const uint8_t*) data, len);
    uint32_t ms = (uint32_t) ((time_us_64() - t0) / 1000ULL);
    if (ms >= 1) {
        remote_log_eventf("BT FLASH-WRITE kind=%s ms=%u", kind, (unsigned) ms);
    }
}

// 【2026-11-13】Flash 書き込みが HCI イベント取りこぼしの原因かの検証用。
// FLASH_STRESS_MS > 0 で、その間隔でダミーの TLV 書き込みを繰り返す。
// 膠着の頻度が上がれば「XIP 停止で CYW43 ドライバが止まる」説が確定する。
// 既定は 0 (無効)。実験時のみ -DFLASH_STRESS_MS=10000 などで有効化する
#ifndef FLASH_STRESS_MS
#define FLASH_STRESS_MS 0
#endif
// 【2026-11-13】IRQ 停止の周期 (ms) と停止時間 (us)。0 で無効
#ifndef STALL_EMU_MS
#define STALL_EMU_MS 0
#endif
#ifndef STALL_EMU_US
#define STALL_EMU_US 10000
#endif
#if FLASH_STRESS_MS > 0
#define TLV_TAG_STRESS 0x53545253u
#endif

static void save_slot_bonded_mask(uint8_t mask) {
    slot_bonded_mask = mask;
    if (!tlv_impl) return;
    tlv_store_timed(TLV_TAG_SLOT_BOND_MASK, &mask, sizeof(mask), "mask");
}

static void save_slot_peers(void) {
    if (!tlv_impl) return;
    tlv_store_timed(TLV_TAG_SLOT_PEERS, slot_peers, sizeof(slot_peers), "peers");
}

static void save_slot_salts(void) {
    if (!tlv_impl) return;
    tlv_store_timed(TLV_TAG_SLOT_SALTS, slot_salts, sizeof(slot_salts), "salts");
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
            uint64_t _t0 = time_us_64();
            int res = tlv_impl->store_tag(&tlv_context, TLV_TAG_CURRENT_SLOT, &slot_to_save, sizeof(slot_to_save));
            if ((time_us_64() - _t0) >= 1000ULL) remote_log_eventf("BT FLASH-WRITE kind=slot ms=%u", (unsigned) ((time_us_64() - _t0) / 1000ULL));
            printf("[BT] Saved current_slot: %d to TLV (res=%d)\r\n", slot_to_save + 1, res);
        }
    }

    if (pending_save_os) {
        pending_save_os = false;
        char saved_os[MAX_SLOTS];
        int len = tlv_impl->get_tag(&tlv_context, TLV_TAG_SLOT_OS, (uint8_t*)saved_os, sizeof(saved_os));
        if (len != sizeof(saved_os) || memcmp(saved_os, slot_os, sizeof(slot_os)) != 0) {
            uint64_t _t1 = time_us_64();
            int res = tlv_impl->store_tag(&tlv_context, TLV_TAG_SLOT_OS, (const uint8_t*)slot_os, sizeof(slot_os));
            if ((time_us_64() - _t1) >= 1000ULL) remote_log_eventf("BT FLASH-WRITE kind=os ms=%u", (unsigned) ((time_us_64() - _t1) / 1000ULL));
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
            uint64_t _t2 = time_us_64();
            int res = tlv_impl->store_tag(&tlv_context, TLV_TAG_RADIO_OFF, &val, sizeof(val));
            if ((time_us_64() - _t2) >= 1000ULL) remote_log_eventf("BT FLASH-WRITE kind=radio ms=%u", (unsigned) ((time_us_64() - _t2) / 1000ULL));
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
// デバイス固有の固定アドレス (スロット・ソルトに依存しない)。
// 【BL-45】CYW43439 は広告アドレスを電源投入時にラッチし、実行中の
// LE Set Random Address が広告に反映されない。スロットごとにアドレスを
// 変える設計をやめ、**1 アドレス + accept list による接続制限**で
// スロットを分離する


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
// 広告の接続フィルタを現在のスロットに合わせる。
//   - ペアリング中 / 未ボンド: 誰でも接続可 (発見してもらう必要がある)
//   - ボンド済み & 非ペアリング: accept list の相手のみ接続可
//     (同じアドレスを複数ホストが知っていても、現在のスロットの相手以外は
//      コントローラが接続を拒否する = スロット分離)
static void update_adv_filter_policy(void) {
#if defined(BL45_NO_WHITELIST) && BL45_NO_WHITELIST
    // 切り分け専用: accept list を触らず policy 0 固定
    uint8_t policy = 0x00;
#else
    gap_whitelist_clear();
    uint8_t policy = 0x00;
    // 参考実装 (EhHHKB2 fw/ble/ble_hid.c) と同じ方式:
    //   policy 0x03 = スキャン要求・接続要求とも accept list の相手のみ。
    //   RPA を使うホストを照合するにはリゾルビングリストが必要なので、
    //   未ロード時はオープン広告 (0x00) にフォールバックする
    if (!is_pairing_mode && slot_peers[current_slot].valid && resolving_list_loaded) {
        gap_whitelist_add((bd_addr_type_t) slot_peers[current_slot].addr_type,
                          slot_peers[current_slot].addr);
        policy = 0x03;
    }
#endif
    bd_addr_t null_addr;
    memset(null_addr, 0, 6);
#if defined(BL45_SIMPLE_ADV) && BL45_SIMPLE_ADV
    // 切り分け専用: 最小構成の広告 (100ms 間隔・policy 0・名前だけ)
    gap_advertisements_set_params(0x00A0, 0x00A0, 0, 0, null_addr, 0x07, 0x00);
#else
    gap_advertisements_set_params(0x0030, 0x0030, 0, 0, null_addr, 0x07, policy);
#endif
    remote_log_eventf("ADV-FILTER slot=%d policy=%d peer_ok=%d", current_slot + 1, policy,
                      slot_peers[current_slot].valid ? 1 : 0);
}

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

    // アドレスは public 固定 (実行中に変更しない)。
    // 代わりに accept list で「現在スロットのボンド相手」だけ接続を許可する
    update_adv_filter_policy();

    // アドバタイズデータ設定
    char name_buf[32];
    snprintf(name_buf, sizeof(name_buf), "Remapper [Slot %d]", current_slot + 1);

    uint8_t adv_data[31];
    int adv_data_len = 0;
#if defined(BL45_SIMPLE_ADV) && BL45_SIMPLE_ADV
    // 切り分け専用: flags + 短い名前だけ
    adv_data[adv_data_len++] = 2;
    adv_data[adv_data_len++] = BLUETOOTH_DATA_TYPE_FLAGS;
    adv_data[adv_data_len++] = 0x06;
    {
        const char* simple_name = "Remap-Test";
        size_t n = strlen(simple_name);
        adv_data[adv_data_len++] = (uint8_t) (n + 1);
        adv_data[adv_data_len++] = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
        memcpy(&adv_data[adv_data_len], simple_name, n);
        adv_data_len += (int) n;
    }
#else

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

#endif
    gap_advertisements_set_data(adv_data_len, adv_data);

    // アドバタイズ有効化は少し遅らせる (disable との同時実行で打ち消されるのを防ぐ)
    adv_enable_pending = true;
    adv_enable_at_us = time_us_64() + 200000ULL;

    bd_addr_t local_addr;
    gap_local_bd_addr(local_addr);
    printf("[BT] <- apply_slot_identity done (state=%d, pairing=%d, bonded=%d). ADDR=%s\r\n",
           current_state, is_pairing_mode, is_bonded, bd_addr_to_str(local_addr));
    remote_log_eventf("APPLY-DONE st=%d pair=%d addr=%s", current_state, is_pairing_mode,
                      bd_addr_to_str(local_addr));
}

// ホットキーから呼ばれる関数
// 同一スロット押下でスロットのON/OFF（電波OFF・切断 / 再接続）をトグルする
void ble_switch_slot(int slot) {
    if (slot < 0 || slot >= MAX_SLOTS) return;
    // 注: ここは ble_lock を取らない。BL-33 の排他は送信経路 (ble_hid_send_report)
    // が対象で、制御経路のロックは BL-43 P1 の仮説として追加されたもの。
    // 実機で「ロック保持中のアドレス変更が広告を壊す」ことが判明したため戻した (BL-45)

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

    ble_switch_slot_locked(slot);
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
            dormant_retry = false; // ユーザー操作の OFF は自動復帰しない
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
    // 制御経路はロックしない (BL-45: ロック保持中の広告設定が効かなくなる)
    ble_enter_pairing_mode_locked(slot);
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
    // 【2026-11-07】HCI イベントが来ているか (チップ/ドライバ生存確認)
    if (packet_type == HCI_EVENT_PACKET) health_last_hci_us = time_us_64();
    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event = hci_event_packet_get_type(packet);

    switch (event) {
        case BTSTACK_EVENT_STATE: {
            uint8_t state = btstack_event_state_get_state(packet);
            printf("[BT] BTSTACK_STATE: %d\r\n", state);
            remote_log_eventf("STACK st=%d", state);
            if (state == HCI_STATE_OFF && ctrl_reset_pending) {
                // クレジット会計ごとリセットする。アドレスは固定なので再設定不要
                ctrl_reset_pending = false;
                bool reload = ctrl_reload_btcore;
                ctrl_reload_btcore = false;
                if (reload) {
                    // トランスポートは閉じている。次の open で BT ファームが
                    // 再ダウンロードされるよう bt_loaded を落としておく
                    cyw43_arch_lwip_begin();
                    cyw43_state.bt_loaded = false;
                    cyw43_arch_lwip_end();
                }
                remote_log_eventf("BT FLOWCTL-RESET ctrl power-on reload=%d", reload ? 1 : 0);
                post_reload = reload;
                adv_verify_deadline_us = 0;
                hci_power_control(HCI_POWER_ON);
                return;
            }
            if (state == HCI_STATE_WORKING) {
                // コントローラ再起動 (FLOWCTL-RESET) 直後は BTstack の広告状態キャッシュが
                // 「有効」のまま残る。そのまま apply すると disable→enable が「変化なし」と
                // 判定されてコマンドが送られず、広告が出ないまま再接続タイムアウトになる。
                // ここで明示的に無効化して、状態変化を作ってから apply させる
                gap_advertisements_enable(0);
                // 広告開始より前にボンド済み IRK をリゾルビングリストへ読み込む
                resolving_list_loaded =
                    gap_load_resolving_list_from_le_device_db() == ERROR_CODE_SUCCESS;
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
                if (post_reload) {
                    post_reload = false;
                    adv_verify_count = 0;
                    adv_verify_deadline_us = time_us_64() + 1500000ULL;
                    remote_log_eventf("BT ADV-VERIFY armed");
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
                    if (can_send_req_us != 0) {
                        uint32_t d = (uint32_t) ((time_us_64() - can_send_req_us) / 1000ULL);
                        if (d > health_lat_max_ms) health_lat_max_ms = d;
                        if (d > health_lat_win_max_ms) health_lat_win_max_ms = d;
                        can_send_req_us = 0;
                        // 許可待ちが 50ms 以上なら時刻付きで残す (15 秒窓の最大値だけでは
                        // いつ起きたか分からない)。1 秒に 1 回までに制限する
                        if (d >= 50u) {
                            static uint64_t last_latgap_us = 0;
                            uint64_t t = time_us_64();
                            if ((t - last_latgap_us) >= 1000000ULL) {
                                last_latgap_us = t;
                                remote_log_eventf("BT LATGAP ms=%u", (unsigned) d);
                            }
                        }
                    }
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
        // コントローラが「送信完了」を返した数。ACL クレジットの返却そのもの
        case HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS: {
            const uint8_t* p = packet + 2; // event type + length の後ろ
            uint8_t n = *p++;
            uint32_t total = 0;
            for (uint8_t i = 0; i < n; i++) {
                p += 2; // connection handle
                total += (uint32_t) p[0] | ((uint32_t) p[1] << 8);
                p += 2;
            }
            health_cp_events++;
            health_cp_packets += total;
            health_last_cp_us = time_us_64();
            break;
        }

        case HCI_EVENT_COMMAND_COMPLETE: {
            uint16_t opcode = hci_event_command_complete_get_command_opcode(packet);
            const uint8_t* params = hci_event_command_complete_get_return_parameters(packet);
            if (opcode == HCI_OPCODE_HCI_LE_SET_ADVERTISE_ENABLE) {
                uint8_t st = params[0];
                adv_enabled_ok = (st == 0) && (adv_enable_req != 0);
                if (st != 0 && adv_enable_req != 0) {
                    adv_retry_pending = true; // 拒否されたときだけ張り直す
                }
                if (st == 0) {
                    // 広告が出た → 監視を解除（チップは生きている）
                    adv_pending_since_us = 0;
                    adv_fail_count = 0;
                    ble_dead_declared = false;
                    ble_dead_snapshot_done = false;
                    ble_reboot_at_us = 0;
                }
                remote_log_eventf("ADV-EN status=0x%02X en=%d slot=%d pair=%d st=%d",
                                  st, adv_enable_req, current_slot + 1, is_pairing_mode,
                                  (int) current_state);
            } else if (opcode == HCI_OPCODE_HCI_LE_SET_RANDOM_ADDRESS) {
                remote_log_eventf("ADV-ADDR status=0x%02X", params[0]);
            } else if (opcode == HCI_OPCODE_HCI_LE_SET_ADVERTISING_PARAMETERS) {
                remote_log_eventf("ADV-PARAM status=0x%02X", params[0]);
            } else if (opcode == HCI_OPCODE_HCI_LE_SET_ADVERTISING_DATA) {
                remote_log_eventf("ADV-DATA status=0x%02X", params[0]);
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
                conn_update_count++;
                printf("[BT] Connection parameters updated: interval=%.2fms, latency=%d, timeout=%dms\r\n",
                       interval * 1.25f, latency, timeout * 10);
                remote_log_eventf("BT PARAM ci=%u lat=%u st=%u n=%u",
                                  (unsigned) interval, (unsigned) latency,
                                  (unsigned) timeout, (unsigned) conn_update_count);

                // 【BL-04】OS自動判別（接続パラメータ・レイテンシ・タイムアウト）
                if (latency > 0) {
                    // Apple (macOS / iOS) は省電力のため latency=4 等を要求
                    update_detected_os('M');
                } else if (timeout >= 450) {
                    // Android のデフォルト supervision timeout は 5000ms (500)
                    peer_has_android_timeout = true;
                    update_detected_os('A');
                }

                // 【2026-10-31】ここでは要求しない。このハンドラは「更新が来た時」しか
                // 走らないため、ホストが既定値を押し付けた直後に 1 回試すだけで、
                // 以後は二度と機会が無い (実測: wake39 で PARAM-REQ が 0 件)。
                // 再要求は周期側 (ble_hid_poll) に移した。ここは結果の記録のみ
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

            resolving_list_loaded = gap_load_resolving_list_from_le_device_db() == ERROR_CODE_SUCCESS;
            // 切断完了時は広告を張り直す (接続中は enable が無視されるため)。
            // ただしユーザーが OFF を指示した切断 (pending_toggle_off) では張り直さない
            if (!(switch_pending && pending_toggle_off)) {
                adv_enable_pending = true;
                adv_enable_at_us = time_us_64() + 200000ULL;
                    // 【2026-11-13】切断完了 → 広告が立ち上がるまでの監視を開始
                adv_pending_since_us = time_us_64();
                proact_pending_us = 0; // PROACT 起因の監視は解除
            }
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
                    adv_enable_pending = false; // 予約済みの広告有効化も取り消す
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
                    // 【2026-11-04】未ボンド (ボンド消去直後など) は広告を出さない。
                    // 以前は切断直後に予約した「200ms 後の広告有効化」がそのまま
                    // 発火し、ボンドを消した直後に旧ホストが再接続して暗号化まで
                    // 通っていた (「Slot Cleared なのにすぐ slot4 で繋がる」)。
                    // 予約を取り消して静音を維持する
                    adv_enable_pending = false;
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
    // アドバタイズはコントローラの public アドレスで行う。
    // 【BL-45】CYW43439 はランダムアドレスを電源投入時にラッチし、実行中の変更が
    // 広告に反映されない。参考実装 (EhHHKB2) もランダムアドレスを使わず、
    // 1 アドレス + accept list でスロットを分離している
    bd_addr_t init_addr;
    gap_local_bd_addr(init_addr);
    remote_log_eventf("BT ADDR public=%s", bd_addr_to_str(init_addr));

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
    // 【BL-45】main ループから毎回 (~25k/s) 呼ばれるため、ここで async ロックを
    // 取らない。高頻度のロック取得が CYW43 側の BT 処理を阻害し、広告コマンドが
    // 受理されても電波に乗らない現象を起こしていた。BTstack API を触る区間
    // (スロット切替・広告の張り直し) だけ個別にロックする
    ble_hid_task_locked();
}

static void ble_hid_task_locked(void) {
    static uint64_t last_countdown_check_us = 0;
    uint64_t now_us = time_us_64();

#if STALL_EMU_MS > 0
    // 【2026-11-13】IRQ 停止ストレス (検証専用・Flash 摩耗なし)。
    // CYW43 のドライバは IRQ で動くので、IRQ を止めれば Flash 書き込み中と
    // 同じ「ホストが止まる」状況を再現できる
    {
        static uint64_t next_stall_us = 0;
        if (next_stall_us == 0) next_stall_us = now_us + (STALL_EMU_MS * 1000ULL);
        if (now_us >= next_stall_us) {
            next_stall_us = now_us + (STALL_EMU_MS * 1000ULL);
            uint32_t irq_state = save_and_disable_interrupts();
            uint64_t t0 = time_us_64();
            while ((time_us_64() - t0) < STALL_EMU_US) {
                tight_loop_contents();
            }
            restore_interrupts(irq_state);
            remote_log_eventf("BT STALL-EMU ms=%u us=%u", (unsigned) STALL_EMU_MS,
                              (unsigned) STALL_EMU_US);
        }
    }
#endif

#if FLASH_STRESS_MS > 0
    // 【2026-11-13】Flash 書き込みストレス (検証専用)
    {
        static uint64_t next_stress_us = 0;
        static uint32_t stress_n = 0;
        if (next_stress_us == 0) next_stress_us = now_us + (FLASH_STRESS_MS * 1000ULL);
        if (now_us >= next_stress_us) {
            next_stress_us = now_us + (FLASH_STRESS_MS * 1000ULL);
            stress_n++;
            if (tlv_impl) {
                uint32_t v = stress_n;
                uint64_t t0 = time_us_64();
                tlv_impl->store_tag(&tlv_context, TLV_TAG_STRESS, (const uint8_t*) &v, sizeof(v));
                remote_log_eventf("BT FLASH-STRESS n=%u ms=%u", (unsigned) stress_n,
                                  (unsigned) ((time_us_64() - t0) / 1000ULL));
            }
        }
    }
#endif

    // 遅延させた広告有効化 (apply_slot_identity / 切断完了から 200ms 後)
    if (adv_enable_pending && now_us >= adv_enable_at_us) {
        adv_enable_pending = false;
        adv_set(1);
    }

#if REPRO_FLOOD_MS > 0
    // 【2026-11-14】再現テスト (改): ゼロ移動のマウスレポートを**送信キューに積む**。
    // 直接 ble_hid_send_report を叩くと許可待ちで自分自身が blk に計上され、
    // 「過負荷」ではなく「壊れた送信者」になってしまう (in=0 でも blk が増えた)。
    // キュー経由なら通常のペーシングで送られ、本物の入力と競合したときだけ溢れる
    if (ble_hid_is_connected() && !ble_hid_is_tx_stalled()) {
        static uint64_t next_flood_us = 0;
        if (next_flood_us == 0) next_flood_us = now_us + (REPRO_FLOOD_MS * 1000ULL);
        if (now_us >= next_flood_us) {
            next_flood_us = now_us + (REPRO_FLOOD_MS * 1000ULL);
            // 空のときだけ積む（自分でキューを埋めない）
            if (or_items == 0) {
                memset(outgoing_reports[or_tail], 0, MAX_REPORT_SIZE + 1);
                outgoing_reports[or_tail][0] = 1; // Report ID 1: Mouse (ゼロ移動)
                or_tail = (uint8_t) ((or_tail + 1) % OR_BUFSIZE);
                or_items++;
            }
        }
    }
#endif

#if REPRO_CYCLE_MS > 0
    // 【2026-11-14】再現テスト: 切断と再広告を交互に繰り返す
    {
        static uint64_t next_repro_us = 0;
        static uint32_t repro_n = 0;
        if (next_repro_us == 0) next_repro_us = now_us + 10000000ULL;
        if (now_us >= next_repro_us) {
            next_repro_us = now_us + (REPRO_CYCLE_MS * 1000ULL);
            repro_n++;
            if (con_handle != HCI_CON_HANDLE_INVALID) {
                remote_log_eventf("BT REPRO n=%u disc up=%us", (unsigned) repro_n,
                                  (unsigned) (now_us / 1000000ULL));
                gap_disconnect(con_handle);
            } else {
                remote_log_eventf("BT REPRO n=%u re-adv up=%us", (unsigned) repro_n,
                                  (unsigned) (now_us / 1000000ULL));
                gap_advertisements_enable(0);
                adv_set(1);
            }
        }
    }
#endif

#if BLE_DEAD_DETECT_ENABLE
    // 【2026-11-12】広告が出ないまま居座る状態（＝チップが HCI を受け付けない）を検出し、
    // 状況を記録したうえで、条件を満たせば自動で再起動する。
    // 再起動のループ防止は「起動後 BLE_AUTO_REBOOT_MIN_UPTIME_S 秒は再起動しない」
    // だけで行う（跨いだ状態は保存しない）
    {
        uint64_t now_d = time_us_64();
        // 【2026-11-13】「切断要求を出した後は広告が立ち上がるはず」という期待で
        // タイマを張る。切断完了イベント自体が来ない（＝チップが固まった）ケースも
        // 捕まえたいので、未接続かどうかでは判定しない。
        // 接続が成立すればその時点でクリアされるので、正常な競合では発動しない
        // 【2026-11-13】PROACT で切断要求を出して 15 秒、切断完了も広告も来ない
        // = コントローラが HCI を受け付けていない。アプリの状態 (CONNECTED のまま)
        // は信用できないので、この経路で判定する
        if (!ble_dead_declared && !hci_recycle_done && !ctrl_reset_pending &&
            ((proact_pending_us != 0 &&
              (now_d - proact_pending_us) > (BLE_PROACT_DEAD_MS * 1000ULL)) ||
             (adv_pending_since_us != 0 &&
              (now_d - adv_pending_since_us) > (BLE_DEAD_ADV_TIMEOUT_MS * 1000ULL)))) {
            // 【2026-11-13】まず HCI 電源の入れ直しでクレジットを復活させる。
            // ファーム再投入はしない (ctrl_reload_btcore = false)
            hci_recycle_done = true;
            adv_pending_since_us = 0;
            proact_pending_us = now_d; // 15 秒の再判定タイマを張り直す
            adv_fail_count = 0;
            remote_log_eventf("BT HCI-RECYCLE ccmd=%d",
                              hci_can_send_command_packet_now() ? 1 : 0);
            ctrl_reset_pending = true;
            ctrl_reload_btcore = false;
            hci_power_control(HCI_POWER_OFF);
            return;
        }

        if (!ble_dead_declared && proact_pending_us != 0 &&
            (now_d - proact_pending_us) > (BLE_PROACT_DEAD_MS * 1000ULL)) {
            ble_dead_declared = true;
            ble_dead_since_us = now_d;
            proact_pending_us = 0;
            adv_pending_since_us = 0;
        } else if (!ble_dead_declared && adv_pending_since_us != 0 &&
                   (now_d - adv_pending_since_us) > (BLE_DEAD_ADV_TIMEOUT_MS * 1000ULL)) {
            if (adv_fail_count < BLE_DEAD_ADV_RETRIES) {
                adv_fail_count++;
                adv_pending_since_us = now_d;
                gap_advertisements_enable(0);
                adv_set(1);
                // 【2026-11-13】ccmd=0 なら BTstack の HCI コマンドクレジットが
                // 枯渇している (= Command Complete を取りこぼした) 証拠
                remote_log_eventf("BT ADV-NOACK retry=%u ccmd=%d",
                                  (unsigned) adv_fail_count,
                                  hci_can_send_command_packet_now() ? 1 : 0);
            } else {
                ble_dead_declared = true;
                ble_dead_since_us = now_d;
                adv_pending_since_us = 0;
            }
        }

        if (ble_dead_declared) {
            if (!ble_dead_snapshot_done) {
                ble_dead_snapshot_done = true;
                ble_dead_wait_log_us = now_d;
                remote_log_eventf(
                    "BT DEAD-SNAP up=%us conn=%u heap=%u temp=%d rssi=%d slot=%d bonded=%d "
                    "ok=%u cp=%u disc=%u rs=0x%02X acl=%u blk=%u adv_ok=%d ccmd=%d",
                    (unsigned) (now_d / 1000000ULL), (unsigned) health_conn_count,
                    (unsigned) remote_log_heap_used(), remote_log_chip_temp_c(),
                    remote_log_last_rssi(), current_slot + 1,
                    is_slot_bonded(current_slot) ? 1 : 0, (unsigned) health_ok,
                    (unsigned) health_cp_packets, (unsigned) health_disc,
                    (unsigned) health_disc_reason, (unsigned) health_acl_full,
                    (unsigned) health_blocked, adv_enabled_ok ? 1 : 0,
                    // 【2026-11-13】HCI コマンドクレジットが残っているか
                    hci_can_send_command_packet_now() ? 1 : 0);
                // 以後 USB で運用しつつ、再起動の可否を待つ
                health_tx_stalled = true;
            }
            if ((now_d / 1000000ULL) >= (uint64_t) BLE_AUTO_REBOOT_MIN_UPTIME_S) {
                if (ble_reboot_at_us == 0) {
                    ble_reboot_at_us = now_d + 500000ULL; // ログ送信の猶予
                    remote_log_eventf("BT AUTO-REBOOT adv_stuck up=%us",
                                      (unsigned) (now_d / 1000000ULL));
                } else if (now_d >= ble_reboot_at_us) {
                    printf("[BT] auto reboot (advertising stuck, uptime=%us)\r\n",
                           (unsigned) (now_d / 1000000ULL));
                    watchdog_reboot(0, 0, 100);
                    while (true) tight_loop_contents();
                }
            } else if ((now_d - ble_dead_wait_log_us) >= 300000000ULL) {
                // まだ再起動できない: 5 分ごとに生存を残す (USB 運用中)
                ble_dead_wait_log_us = now_d;
                remote_log_eventf("BT DEAD-WAIT up=%us (reboot at %us)",
                                  (unsigned) (now_d / 1000000ULL),
                                  (unsigned) BLE_AUTO_REBOOT_MIN_UPTIME_S);
            }
        }
    }
#endif

    // コントローラ再投入後に「広告が本当に出たか」を確認する。
    // ADV-EN の完了通知が返らないまま WORKING 到達 = 復旧と判断して休眠に入り、
    // 実際には何も飛んでいない事故があった (2026-10-31 00:44)。1.5 秒待って
    // ADV-EN が無ければ張り直し、3 回失敗したら諦めて電源再投入を促す
    if (adv_verify_deadline_us != 0 && now_us >= adv_verify_deadline_us) {
        if (adv_enabled_ok) {
            adv_verify_deadline_us = 0;
            remote_log_eventf("BT ADV-VERIFY ok n=%u", (unsigned) adv_verify_count);
        } else if (adv_verify_count < 3) {
            adv_verify_count++;
            adv_verify_deadline_us = now_us + 1500000ULL;
            gap_advertisements_enable(0);
            adv_set(1);
            remote_log_eventf("BT ADV-RETRY n=%u", (unsigned) adv_verify_count);
        } else {
            adv_verify_deadline_us = 0;
            remote_log_eventf("BT CTRL-DEAD adv never enabled -> power cycle needed");
            oled_set_status("Power Cycle!");
            oled_show_toast("POWER CYCLE", 6000);
        }
    }

    // 【BL-43 系】ACL バッファ満杯 (0x57) の連発で通知が止まったらコントローラを再起動する。
    // 再接続ではクレジットが戻らないため、HCI 電源の入れ直しだけが回復手段
    {
        static uint32_t last_ok = 0;
        static uint32_t acl_at_last_ok = 0;
        static uint64_t last_ok_us = 0;
        static bool was_connected = false;
        static uint64_t cooldown_until_us = 0;
        static uint64_t connected_since_us = 0;
        static uint8_t param_req_count = 0;
        static uint64_t last_param_req_us = 0;
        // 接続した瞬間の Completed Packets。ここから動かなければクレジットが
        // 返っていない = 壊れた再接続 (2026-10-31 00:42 実測)
        static uint32_t cp_at_connect = 0;
        // 【2026-11-05】Completed Packets が最後に動いた時刻。0x57 のストームを
        // 絞ると acl (0x57 回数) が伸びなくなり、acl 依存の膠着判定が飢えて
        // 11 分間復旧しなかった。cp の停滞そのものを判定に使う
        static uint32_t cp_last = 0;
        static uint64_t cp_last_change_us = 0;
        // 接続した瞬間は判定の基準を取り直す。前回接続時の経過時間のままだと
        // 「接続直後に停止」と誤判定して再起動を連鎖させる (2026-10-24 朝に実測)
        bool connected_now = (current_state == BLE_STATE_CONNECTED);
        if (connected_now && !was_connected) {
            last_ok = health_ok;
            acl_at_last_ok = health_acl_full;
            last_ok_us = now_us;
            connected_since_us = now_us;
            credit_stall = false;
            consecutive_can_send_timeouts = 0;
            param_req_count = 0;
            // 【2026-11-07】接続直後はホスト既定 (st=1 秒) に戻っているので、
            // 30 秒待たず **2 秒後** に 1 回目の再要求を出す。30 秒待つ間に
            // 1 秒の監視窓で切れて再接続…を繰り返す「弱い状態のループ」に
            // 入っていた (21:57 実測: 再接続 → 0.5〜2 秒で rs=0x08)
            if (now_us > 28000000ULL) {
                last_param_req_us = now_us - 28000000ULL;
            } else {
                last_param_req_us = 0;
            }
            cp_at_connect = health_cp_packets;
            cp_last = health_cp_packets;
            cp_last_change_us = now_us;
            // 再接続直後に前の接続の連続 0x57 を引きずらない
            // (2026-11-05: 再接続 20ms 後に stale な aclrun=51 で誤って絞った)
            health_acl_run = 0;
            health_conn_count++;
            // 接続した = 広告は不要。ADV 監視も解除する
            adv_pending_since_us = 0;
            adv_fail_count = 0;
            ble_dead_declared = false;
            ble_dead_snapshot_done = false;
            ble_reboot_at_us = 0;
            proact_pending_us = 0;
            hci_recycle_done = false;
            // 【2026-11-14】接続が成立したら tx_stalled を解除する。
            // これが残っていると転送先が USB に固定され、BLE では一度も送らないため
            // 「送信成功で解除」の条件が永久に成立しない（BLE が健全でも USB-OUT の
            // ままになる）。新しいリンクなので一度は試す
            health_tx_stalled = false;
        }
        was_connected = connected_now;

        if (health_cp_packets != cp_last) {
            cp_last = health_cp_packets;
            cp_last_change_us = now_us;
        }

        // 【2026-10-31 00:42 実測】接続直後に 0x57 が連続し cp が凍結する
        // 「壊れた再接続」では CAN_SEND_NOW は届き続けるため、タイマー側の
        // credit_stall 判定が発動しなかった (1261 回の 0x57 を撒いた)。
        // 0x57 の連続数 (aclrun) で直接検出して送信を 1 秒 1 回に絞る
        if (connected_now && !credit_stall && health_acl_run >= 50) {
            credit_stall = true;
            credit_stall_next_probe_us = now_us + 1000000ULL;
            remote_log_eventf("BT CREDIT-STALL start (storm aclrun=%u acl=%u cpd=%u)",
                              (unsigned) health_acl_run,
                              (unsigned) (health_acl_full - acl_at_last_ok),
                              (unsigned) (health_cp_packets - cp_at_connect));
        }

        if (health_ok != last_ok) {
            last_ok = health_ok;
            acl_at_last_ok = health_acl_full;
            last_ok_us = now_us;
            health_tx_stalled = false;
            credit_stall = false; // クレジットが戻った
            proact_pending_us = 0; // 送信が戻った = チップは生きている
        } else if (connected_now && !ctrl_reset_pending && now_us >= cooldown_until_us &&
                   (now_us - connected_since_us) > 15000000ULL &&
                   (((health_acl_full - acl_at_last_ok) >= 40 &&
                     (now_us - last_ok_us) > 120000000ULL) ||
                    // 絞っても (1 秒 1 回のプローブでも) クレジットが返らないまま
                    // 20 秒 = 壊れた再接続。0x57 を撒き続ける前にコントローラを
                    // 入れ直す (2026-10-31 00:42 は 120 秒待つ間に 1261 回撒いて
                    // コントローラが広告コマンドを受け付けなくなった)
                    (credit_stall && (now_us - cp_last_change_us) > 20000000ULL))) {
            // しきい値の変遷 (2026-10-29〜30 実測):
            //   3s/20  … Win→Mac 切替直後の初期バーストで誤爆した
            //   10s/40 … 眠っている Mac に接続した時の 0x57 詰まりでも発動した
            //   120s   … ホストのスリープ中は「待てば戻る」ので、まず待つ
            //
            // 【重要】2026-10-30: ここで hci_power_control(HCI_POWER_OFF/ON) を
            // すると、CYW43 は復帰後に HCI コマンドを受け付けなくなる
            // (BTstack 側は次のコマンドを投げるが ADV-PARAM / ADV-EN の完了が
            // 一度も返らず、広告が出ないまま復旧不能)。実測 3 回とも復旧は Pico の
            // 電源入れ直しのみだった。よって電源再投入は廃止し、
            // 「切断 → 通常の再広告」で復帰させる (コントローラは生かしたまま)
            cooldown_until_us = now_us + 60000000ULL; // 連鎖を防ぐ 60 秒クールダウン
            health_tx_stalled = true;                 // 以後 USB フォールバックに切り替える
            if ((now_us - cp_last_change_us) > 20000000ULL) {
                remote_log_eventf("BT NORESTORE cp frozen %us acl=%u -> btcore-reload",
                                  (unsigned) ((now_us - cp_last_change_us) / 1000000ULL),
                                  (unsigned) (health_acl_full - acl_at_last_ok));
            } else {
                remote_log_eventf("BT FLOWCTL-WEDGE ok_stall=%us acl=%u -> btcore-reload",
                                  (unsigned) ((now_us - last_ok_us) / 1000000ULL),
                                  (unsigned) (health_acl_full - acl_at_last_ok));
            }
            // 膠着すると CYW43 は HCI コマンドを受け付けなくなる。
            // BTstack を一度落とし (HCI_STATE_OFF)、その状態で BT コアのファームを
            // 再投入してから電源 ON に戻す。トランスポートが閉じている間に
            // 再ダウンロードさせるのが肝 (開いたまま触ると初期化が完了しなかった)
            ctrl_reset_pending = true;
            ctrl_reload_btcore = true;
            hci_power_control(HCI_POWER_OFF);
            return;
        }

        // 【2026-11-03】lat 高止まりでの自発再接続。
        // CAN_SEND_NOW 待ち (lat) が 30ms 以上で 60 秒続くのは、コントローラが
        // 送信スロットを回せなくなってきた合図 (15:26 の膠着では 6 分前から
        // 36〜50ms が続いていた)。固まりきる前に一度きれいに切断して張り直す
#if BLE_LAT_WATCH_ENABLE
        {
            static uint64_t lat_win_us = 0;
            static uint8_t lat_high_windows = 0;
            if (connected_now && !ctrl_reset_pending &&
                (now_us - connected_since_us) > 30000000ULL) {
                if (lat_win_us == 0) lat_win_us = now_us;
                if ((now_us - lat_win_us) >= 10000000ULL) {
                    uint32_t win_max = health_lat_win_max_ms;
                    health_lat_win_max_ms = 0;
                    lat_win_us = now_us;
                    if (win_max >= BLE_LAT_HIGH_MS) {
                        if (lat_high_windows < 0xFF) lat_high_windows++;
                    } else {
                        lat_high_windows = 0;
                    }
                    if (lat_high_windows >= BLE_LAT_HIGH_WINDOWS) {
                        lat_high_windows = 0;
                        remote_log_eventf("BT LAT-HIGH %ums x%u -> proactive reconnect",
                                          (unsigned) win_max,
                                          (unsigned) BLE_LAT_HIGH_WINDOWS);
                        gap_disconnect(con_handle);
                        return;
                    }
                }
            } else {
                lat_win_us = 0;
                lat_high_windows = 0;
            }
        }
#endif

        // 【2026-10-31】接続パラメータの再要求。ホスト (macOS) は既定の
        // (12, 4, 100) = 15ms / latency 4 / supervision timeout 1.0 秒 を押し付ける。
        // timeout 1 秒は取りこぼし 13 回で切断 (rs=0x08) になるため、
        // Apple 準拠の値で出し直せるか試す。1 回目は 15ms、2 回目以降は
        // 11.25ms (Apple が HID 向けに許す下限)。30 秒間隔で最大 3 回。
        // 受理されたかは BT PARAM (更新完了ログ) の st= で判定する
        // 復帰直後 (60 秒) は撃たない。スリープ明けはリンクが不安定で、
        // パラメータ更新要求の直後に rs=0x08 が続いた実測がある (2026-11-01 13:20)
        if (connected_now && ble_conn_latency > 0 && param_req_count < 3 &&
            (now_us - last_param_req_us) >= 30000000ULL &&
            (last_resume_us == 0 || (now_us - last_resume_us) >= 60000000ULL)) {
            last_param_req_us = now_us;
            param_req_count++;
            uint16_t req_ci = (param_req_count == 1) ? 12 : 9;
            printf("[BT] Re-requesting connection params n=%u ci=%u (now lat=%u st=%u)\r\n",
                   (unsigned) param_req_count, (unsigned) req_ci,
                   (unsigned) ble_conn_latency, (unsigned) ble_conn_timeout);
            remote_log_eventf("BT PARAM-REQ n=%u ci=%u lat=0 to=4000 (now lat=%u st=%u)",
                              (unsigned) param_req_count, (unsigned) req_ci,
                              (unsigned) ble_conn_latency, (unsigned) ble_conn_timeout);
            gap_request_connection_parameter_update(con_handle, req_ci, req_ci, 0, 400);
        }
    }

    // 広告の張り直し (BL-45): コントローラが明示的に拒否した (status != 0) 場合のみ。
    // 「まだ受理が確認できていない」だけで再要求すると、アドレス/パラメータ設定の
    // 途中で広告を有効にしてしまい、以降の設定が反映されない
    if (adv_retry_pending && con_handle == HCI_CON_HANDLE_INVALID &&
        (current_state == BLE_STATE_PAIRING || current_state == BLE_STATE_RECONNECTING) &&
        now_us >= adv_retry_next_us) {
        adv_retry_pending = false;
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
            // 未接続状態で別スロットへの切り替え、またはペアリングモード開始。
            // 【BL-45】ここは main ループが ble_lock (cyw43 async lock) を保持している。
            // この状態で BTstack の広告系 API を直接呼ぶと、コマンドは受理される
            // (command complete = 0x00) のに電波に乗らず、ホストから発見できない。
            // ブート時の適用 (STACK WORKING) は async コンテキストから走って動くため、
            // 同じ経路 = slot_transition タイマ (async) に乗せて実行させる
            switch_pending = false;
            schedule_slot_transition(slot, is_pairing);
        }
    }
}

bool ble_hid_is_connected(void) {
    return current_state == BLE_STATE_CONNECTED;
}

// 接続中でも送信が死んでいる (0x57 枯渇) なら false を返す。
// 呼び出し側 (トランスポート選択) はこれで USB フォールバックへ落とせる
bool ble_hid_is_tx_stalled(void) {
    return health_tx_stalled;
}

bool ble_hid_is_dormant(void) {
    return dormant_retry;
}

// 休眠 (再接続タイムアウト後の電波OFF) から、入力イベントを契機に接続を試みる。
// 連続入力で叩かれ続けないよう 5 秒間隔に制限する
void ble_hid_wake_from_dormant(void) {
    if (!dormant_retry) return;
    // すでに広告を出している (RECONNECTING) なら触らない。
    // 毎回 apply すると広告が点滅してホストが接続できない (2026-10-24 実測)
    if (current_state != BLE_STATE_STANDBY_SILENT) return;
    uint64_t now_us = time_us_64();
    if (now_us - dormant_last_try_us < 5000000ULL) return;
    dormant_last_try_us = now_us;
    dormant_retry = false; // 1 回だけ試す (失敗すればタイムアウトで再度休眠に入る)
    btstack_run_loop_remove_timer(&reconnect_timeout_timer);
    oled_set_radio_off(false);
    ble_lock();
    current_state = BLE_STATE_RECONNECTING;
    update_ui_for_state(current_state);
    apply_slot_identity(current_slot); // 広告を張り直し、30 秒タイマも再武装される
    ble_unlock();
    remote_log_eventf("BT WAKE-RETRY slot=%d", current_slot + 1);
}

bool ble_hid_is_pairing(void) {
    return is_pairing_mode || (current_state == BLE_STATE_PAIRING);
}

bool ble_hid_can_send(void) {
    return (current_state == BLE_STATE_CONNECTED) &&
           (con_handle != HCI_CON_HANDLE_INVALID) &&
           ble_can_send &&
           !slot_disconnect_after_zero;
}

void ble_hid_send_report(uint8_t report_id, const uint8_t* data, uint8_t len) {
    if (current_state != BLE_STATE_CONNECTED || con_handle == HCI_CON_HANDLE_INVALID) {
        health_skip_state++;
        return;
    }

    // 【BL-15】暗号化完了 かつ 該当レポート購読完了を厳密にチェック
    if (!ble_encrypted) {
        health_skip_enc++;
        return;
    }
    if (report_id == 2 && !ble_kbd_ready) {      // Report ID 2: Keyboard
        health_skip_ready++;
        return;
    }
    if (report_id == 1 && !ble_mouse_ready) {    // Report ID 1: Mouse
        health_skip_ready++;
        return;
    }
    if (report_id == 3 && !ble_consumer_ready) { // Report ID 3: Consumer (BL-12)
        health_skip_ready++;
        return;
    }
    if (report_id != 1 && report_id != 2 && report_id != 3) return;

    if (!ble_can_send) {
        health_blocked++; // フロー制御待ちで捨てた (ウェッジ時に増える)
        return;
    }
    if (credit_stall) {
        // クレジットが返らない間は 1 秒に 1 回だけ試す。0x57 の連打を止める
        uint64_t nowp = time_us_64();
        if (nowp < credit_stall_next_probe_us) {
            health_blocked++;
            return;
        }
        credit_stall_next_probe_us = nowp + 1000000ULL;
    }

    ble_lock();

    oled_notify_tx();
    uint8_t status = hids_device_send_input_report_for_id(con_handle, report_id, (uint8_t*)data, len);
    if (status == ERROR_CODE_SUCCESS) {
        // 「放置 → 触った」という遷移のマーカー (2026-10-26 追加)。
        // 11 時間放置しても切断しない一方で、操作を再開した直後や BT スイッチ後に
        // 不安定になるという観測があるため、入力再開の時刻を明示的に残す
        // (直後に DISC が出れば引き金が「再開」だと確定できる)
        static uint64_t last_send_us = 0;
        const uint64_t now_us = time_us_64();
        if (last_send_us != 0 && (now_us - last_send_us) < 2000ULL) {
            health_burst++; // 2ms 未満の連続送信 = 1 接続イベントへの詰め込み
        }
        if (last_send_us != 0 && (now_us - last_send_us) >= 10000000ULL) {
            remote_log_eventf("BT RESUME gap=%us", (unsigned) ((now_us - last_send_us) / 1000000ULL));
            last_resume_us = now_us;
        }
        last_send_us = now_us;
        health_ok++;
        health_last_ok_us = now_us;
        health_acl_run = 0; // 通ったので連続 0x57 は途切れた
    } else if (status == 0x57) { // BTSTACK_ACL_BUFFERS_FULL: 受理されず再要求になる
        health_acl_full++;
        if (health_acl_run < 0xFFFF) health_acl_run++;
    }
    if (status == ERROR_CODE_SUCCESS || status == 0x57) { // 0x57: BTSTACK_ACL_BUFFERS_FULL
        ble_can_send = false;
        btstack_run_loop_remove_timer(&can_send_timeout_timer);
        btstack_run_loop_set_timer_handler(&can_send_timeout_timer, can_send_timeout_handler);
        btstack_run_loop_set_timer(&can_send_timeout_timer, 2000); // 2秒の安全ウォッチドッグ
        btstack_run_loop_add_timer(&can_send_timeout_timer);
        can_send_req_us = time_us_64(); // 「いつ許可を求めたか」を残す
        uint8_t req_err = hids_device_request_can_send_now_event(con_handle);
        if (req_err != ERROR_CODE_SUCCESS) {
            can_send_req_us = 0;
            ble_can_send = true;
            btstack_run_loop_remove_timer(&can_send_timeout_timer);
        }
    } else {
        health_err++; // ok が伸びない理由を HB の err= で判別できるようにする
        printf("[BLE-ERR] send_report id=%d len=%d status=0x%02X\r\n", report_id, len, status);
        ble_can_send = true;
    }

    ble_unlock();
}


void ble_hid_kick_reconnect(void) {
    ble_hid_kick_reconnect_locked();
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
    // 制御経路はロックしない (BL-45)
    ble_clear_slot_bonds_locked(slot);
}

void ble_clear_bonds(void) {
    ble_clear_bonds_locked();
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

static uint64_t last_mouse_pace_us = 0;
static uint8_t last_mouse_buttons = 0;

extern bool send_report(ble_send_report_fn_t do_send_report);

static void send_all_release_reports(void) {
    // USB 側にキーボード全離下を送信
    usb_clear_kbd_report();

    // BLE 側にも全離下を送信 (キーボード/マウス/コンシューマ)。
    // マウスを忘れると「左クリック押しっぱなし」等の固着が残る
    if (ble_hid_is_connected()) {
        uint8_t zero_kbd[16] = {0};
        uint8_t zero_mouse[9] = {0};
        uint8_t zero_consumer[1] = {0};
        ble_hid_send_report(2, zero_kbd, sizeof(zero_kbd));
        ble_hid_send_report(1, zero_mouse, sizeof(zero_mouse));
        ble_hid_send_report(3, zero_consumer, sizeof(zero_consumer));
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
        const uint64_t prev_pace_us = last_mouse_pace_us;
        last_mouse_pace_us = time_us_64();
        last_mouse_buttons = report_data[0];
        // 相対移動は 1 レポートに集約される。停止中に溜まった移動が一気に
        // 送られると「カーソルが飛ぶ」ので、大きいデルタは時刻付きで残す
        static uint64_t last_bigdx_us = 0;
        int32_t dx = (int16_t) (report_data[1] | (report_data[2] << 8));
        int32_t dy = (int16_t) (report_data[3] | (report_data[4] << 8));
        const uint64_t now_us = time_us_64();
        {
            uint32_t d = (uint32_t) ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy));
            if (d > health_dmax) health_dmax = d;
        }
        // 「しばらく送れず、溜めた移動を 1 発で送った」瞬間だけを拾う
        // (速く動かしただけの大きいデルタと区別するため、前回送信から 100ms 以上空いた場合に限る)
        if ((dx >= 40 || dx <= -40 || dy >= 40 || dy <= -40) &&
            prev_pace_us != 0 && (now_us - prev_pace_us) >= 100000ULL &&
            (now_us - last_bigdx_us) >= 1000000ULL) {
            last_bigdx_us = now_us;
            remote_log_eventf("BT BIGDX dx=%d dy=%d", (int) dx, (int) dy);
        }
    }

    ble_hid_send_report(report_id, report_data, report_len);
    return true;
}

void ble_transport_task(ble_send_report_fn_t do_send_usb) {
    // 休眠中でも、入力があれば BLE 接続を試みる (USB 転送はそのまま続ける)
    if (or_items > 0 && dormant_retry) {
        ble_hid_wake_from_dormant();
    }
    static bool prev_ble_connected = false;
    bool curr_ble_connected = ble_hid_is_connected() && !ble_hid_is_tx_stalled();
    if (curr_ble_connected != prev_ble_connected) {
        prev_ble_connected = curr_ble_connected;
        printf("[BT] Active transport changed: %s\r\n", curr_ble_connected ? "BLE" : "USB");
        remote_log_eventf("BT XPORT ble=%u", curr_ble_connected ? 1 : 0);
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
                    // 1 接続インターバルに 1 パケットへ揃えるための最低間隔。
                    // 既定は interval - 500us (15ms 間隔なら 14.5ms)。
                    // BLE_MOUSE_PACE_MARGIN_US を大きくすると間隔が詰まり、通知レート
                    // (ok/s) が上がる。バースト衝突が再発しないかは gmax/tx で確認する
#if defined(BLE_MOUSE_PACE_MARGIN_US)
                    const uint32_t pace_margin = (uint32_t) BLE_MOUSE_PACE_MARGIN_US;
#else
                    const uint32_t pace_margin = 500;
#endif
                    uint32_t pace_us = (interval_us > pace_margin + 1000)
                                           ? (interval_us - pace_margin)
                                           : 14500;
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
