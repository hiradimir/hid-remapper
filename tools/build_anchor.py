#!/usr/bin/env python3
"""3a8d85b (BL-45 アンカー) に、今日の修正・観測をすべて載せるパッチ。

使い方 (リポジトリのルートで):
  WT=/private/var/folders/.../opencode/wt_anchor
  git worktree prune && rm -rf $WT && git worktree add $WT 3a8d85b
  cp firmware/src/remote_log_net.h $WT/firmware/src/     # secret (読み書き注意)
  python3 tools/build_anchor.py $WT
  cd $WT/firmware
  R=<リポジトリ>/firmware
  for d in pico-sdk tinyusb Pico-PIO-USB usb_midi_host; do ln -s $R/$d .; done
  mkdir -p build && cd build
  cmake .. -DPICO_BOARD=pico_w -DPICO_DUAL_PIO_USB_DP_PIN=2 -DENABLE_BLE=ON \
        -DENABLE_OLED=ON -DENABLE_REMOTE_LOG=ON -DBLE_LOCK_DISABLE=ON
  make -j$(sysctl -n hw.ncpu) remapper
  # 焼く前に検算:
  #   strings build/remapper.uf2 | grep -o "ble_lock=[a-z]*"   -> disabled
  #   strings build/remapper.uf2 | grep -c FLOWCTL-WEDGE       -> 1 以上
  #   strings build/remapper.uf2 | grep -c 192.168.215.1       -> 0 (誤設定検出)
"""
import sys

WT = sys.argv[1]


def patch(path, subs):
    s = open(path).read()
    for a, b in subs:
        assert s.count(a) == 1, (path, s.count(a), a[:60])
        s = s.replace(a, b)
    open(path, 'w').write(s)


# --- ble_hid.cc -------------------------------------------------------------
patch(WT + '/firmware/src/ble_hid.cc', [
    # ペーシングは本番ビルドと同じ既定 (interval - 500us = 14.5ms) に揃える
    ('#include "btstack_tlv_flash_bank.h"',
     '// ペーシング: 既定 (interval - 500us = 14.5ms)。本番ビルドと条件を揃える\n'
     '#include "btstack_tlv_flash_bank.h"'),

    # 状態変数
    ("static volatile int adv_enable_req = 0;",
     "static volatile bool ctrl_reset_pending = false;   // 0x57 膠着からの復旧\n"
     "static volatile bool health_tx_stalled = false;   // 接続中だが送信不能\n"
     "static volatile bool adv_enable_pending = false;  // 遅延させる広告有効化\n"
     "static uint64_t adv_enable_at_us = 0;\n"
     "static volatile bool dormant_retry = false;       // 再接続タイムアウト後の電波OFF\n"
     "static uint64_t dormant_last_try_us = 0;\n"
     "\n"
     "static volatile int adv_enable_req = 0;"),

    ("static volatile uint32_t health_blocked = 0;",
     "static volatile uint32_t health_blocked = 0;\n"
     "static volatile uint32_t health_cp_events = 0;\n"
     "static volatile uint32_t health_cp_packets = 0;"),

    ("    out->blocked = health_blocked;",
     "    out->blocked = health_blocked;\n"
     "    out->cp_events = health_cp_events;\n"
     "    out->cp_packets = health_cp_packets;"),

    # 遅延広告 + 0x57 watchdog (タスク先頭)
    ("    // 広告の張り直し (BL-45): 要求は出したのにコントローラが受理していない、",
     """    // 遅延させた広告有効化 (disable と同時だと打ち消されるため 200ms 後)
    if (adv_enable_pending && now_us >= adv_enable_at_us) {
        adv_enable_pending = false;
        adv_set(1);
    }

    // 0x57 (ACL バッファ満杯) の連発で通知が止まったらコントローラを再起動する。
    // ただしホストのスリープ中も同じ症状になるため、しきい値は 120 秒と長め
    // (2026-10-30: 10 秒で再起動したら Mac が再接続しなくなった実害 3 回)
    {
        static uint32_t last_ok = 0;
        static uint32_t acl_at_last_ok = 0;
        static uint64_t last_ok_us = 0;
        static bool was_connected = false;
        static uint64_t cooldown_until_us = 0;
        static uint64_t connected_since_us = 0;
        bool connected_now = (current_state == BLE_STATE_CONNECTED);
        if (connected_now && !was_connected) {
            last_ok = health_ok;
            acl_at_last_ok = health_acl_full;
            last_ok_us = now_us;
            connected_since_us = now_us;
        }
        was_connected = connected_now;

        if (health_ok != last_ok) {
            last_ok = health_ok;
            acl_at_last_ok = health_acl_full;
            last_ok_us = now_us;
            health_tx_stalled = false;
        } else if (connected_now && !ctrl_reset_pending && now_us >= cooldown_until_us &&
                   (health_acl_full - acl_at_last_ok) >= 40 &&
                   (now_us - last_ok_us) > 120000000ULL &&
                   (now_us - connected_since_us) > 15000000ULL) {
            // CYW43 は HCI 電源再投入後にコマンドを受け付けなくなり復旧不能
            // (2026-10-30 実測 3 回)。切断 → 通常の再広告で復帰させる
            cooldown_until_us = now_us + 60000000ULL;
            health_tx_stalled = true;
            remote_log_eventf("BT FLOWCTL-WEDGE ok_stall=%us acl=%u -> disconnect",
                              (unsigned) ((now_us - last_ok_us) / 1000000ULL),
                              (unsigned) (health_acl_full - acl_at_last_ok));
            gap_disconnect(con_handle);
            return;
        }
    }

    // 広告の張り直し (BL-45): 要求は出したのにコントローラが受理していない、"""),

    # 有効化を遅延 (apply_slot_identity 末尾)
    ("""    // アドバタイズ即時有効化
    adv_set(1);""",
     """    // アドバタイズ有効化は 200ms 遅らせる
    adv_enable_pending = true;
    adv_enable_at_us = time_us_64() + 200000ULL;"""),

    # タイムアウトで休眠
    ("""        remote_log_eventf("RECONN-TO radio-off");
        adv_set(0);
        current_state = BLE_STATE_STANDBY_SILENT;""",
     """        remote_log_eventf("RECONN-TO radio-off (dormant)");
        adv_set(0);
        dormant_retry = true;
        current_state = BLE_STATE_STANDBY_SILENT;"""),

    # 明示 OFF では休眠にしない
    ("""            current_state = BLE_STATE_STANDBY_SILENT;
            update_ui_for_state(current_state);
            oled_set_radio_off(true);""",
     """            dormant_retry = false;
            current_state = BLE_STATE_STANDBY_SILENT;
            update_ui_for_state(current_state);
            oled_set_radio_off(true);"""),

    # 接続で休眠解除
    ('        printf("[BT] Connection stabilized -> CONNECTED (Ready to send)\\r\\n");',
     '        printf("[BT] Connection stabilized -> CONNECTED (Ready to send)\\r\\n");\n'
     '        dormant_retry = false;'),

    # 公開 API
    ("""bool ble_hid_is_connected(void) {
    return current_state == BLE_STATE_CONNECTED;
}""",
     """bool ble_hid_is_connected(void) {
    return current_state == BLE_STATE_CONNECTED;
}

bool ble_hid_is_tx_stalled(void) {
    return health_tx_stalled;
}

bool ble_hid_is_dormant(void) {
    return dormant_retry;
}

void ble_hid_wake_from_dormant(void) {
    if (!dormant_retry) return;
    if (current_state != BLE_STATE_STANDBY_SILENT) return;
    uint64_t now_us = time_us_64();
    if (now_us - dormant_last_try_us < 5000000ULL) return;
    dormant_last_try_us = now_us;
    dormant_retry = false;
    btstack_run_loop_remove_timer(&reconnect_timeout_timer);
    ble_lock();
    current_state = BLE_STATE_RECONNECTING;
    update_ui_for_state(current_state);
    apply_slot_identity(current_slot);
    ble_unlock();
    remote_log_eventf("BT WAKE-RETRY slot=%d", current_slot + 1);
}"""),

    # トランスポート選択
    ("    bool curr_ble_connected = ble_hid_is_connected();",
     "    bool curr_ble_connected = ble_hid_is_connected() && !ble_hid_is_tx_stalled();"),

    # transport 入口で休眠復帰
    ("""void ble_transport_task(ble_send_report_fn_t do_send_usb) {
    static bool prev_ble_connected = false;""",
     """void ble_transport_task(ble_send_report_fn_t do_send_usb) {
    if (or_items > 0 && dormant_retry) {
        ble_hid_wake_from_dormant();
    }
    static bool prev_ble_connected = false;"""),

    # STACK: 復旧時の電源ON + 広告状態のリセット
    ('            remote_log_eventf("STACK st=%d", state);',
     """            remote_log_eventf("STACK st=%d", state);
            if (state == HCI_STATE_OFF && ctrl_reset_pending) {
                remote_log_eventf("BT FLOWCTL-RESET ctrl power-on");
                ctrl_reset_pending = false;
                hci_power_control(HCI_POWER_ON);
                return;
            }"""),

    ("            if (state == HCI_STATE_WORKING) {",
     """            if (state == HCI_STATE_WORKING) {
                gap_advertisements_enable(0);"""),

    # 切断完了でも広告を張り直す (ユーザーの OFF 指示時は除く)
    ("            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);",
     """            if (!(switch_pending && pending_toggle_off)) {
                adv_enable_pending = true;
                adv_enable_at_us = time_us_64() + 200000ULL;
            }
            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);"""),
    ("""                if (pending_toggle_off) {
                    pending_toggle_off = false;
                    is_pairing_mode = false;
                    adv_set(0);""",
     """                if (pending_toggle_off) {
                    pending_toggle_off = false;
                    is_pairing_mode = false;
                    adv_enable_pending = false;
                    adv_set(0);"""),

    # Completed Packets の観測
    ("        case HCI_EVENT_COMMAND_COMPLETE: {",
     """        case HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS: {
            const uint8_t* p = packet + 2;
            uint8_t n = *p++;
            uint32_t total = 0;
            for (uint8_t i = 0; i < n; i++) {
                p += 2;
                total += (uint32_t) p[0] | ((uint32_t) p[1] << 8);
                p += 2;
            }
            health_cp_events++;
            health_cp_packets += total;
            break;
        }

        case HCI_EVENT_COMMAND_COMPLETE: {"""),

    # マウスペーシング
    ("                    uint32_t pace_us = (interval_us > 500) ? (interval_us - 500) : 14500;",
     """#if defined(BLE_MOUSE_PACE_MARGIN_US)
                    const uint32_t pace_margin = (uint32_t) BLE_MOUSE_PACE_MARGIN_US;
#else
                    const uint32_t pace_margin = 500;
#endif
                    uint32_t pace_us = (interval_us > pace_margin + 1000)
                                           ? (interval_us - pace_margin)
                                           : 14500;"""),
])

# --- ble_hid.h --------------------------------------------------------------
patch(WT + '/firmware/src/ble_hid.h', [
    ("bool ble_hid_is_connected(void);",
     "bool ble_hid_is_connected(void);\n"
     "bool ble_hid_is_tx_stalled(void);\n"
     "bool ble_hid_is_dormant(void);\n"
     "void ble_hid_wake_from_dormant(void);"),
    ("static inline bool ble_hid_is_connected(void) { return false; }",
     "static inline bool ble_hid_is_connected(void) { return false; }\n"
     "static inline bool ble_hid_is_tx_stalled(void) { return false; }\n"
     "static inline bool ble_hid_is_dormant(void) { return false; }\n"
     "static inline void ble_hid_wake_from_dormant(void) {}"),
    ("    out->blocked = 0;",
     "    out->blocked = 0;\n    out->cp_events = 0;\n    out->cp_packets = 0;"),
    ("bool usb_is_ready(void);",
     "bool usb_is_ready(void);\nbool usb_is_suspended(void);"),
])

# 構造体に cp フィールドを足す
p = WT + '/firmware/src/ble_hid.h'
s = open(p).read()
anchor = "    uint32_t blocked;"
i = s.index(anchor)
j = s.index('\n', i)
s = s[:j + 1] + "    uint32_t cp_events;\n    uint32_t cp_packets;\n" + s[j + 1:]
open(p, 'w').write(s)

# --- main.cc ----------------------------------------------------------------
patch(WT + '/firmware/src/main.cc', [
    ("""extern "C" bool usb_is_ready(void) {
    return tud_hid_n_ready(0) || tud_suspended();
}""",
     """extern "C" bool usb_is_ready(void) {
    return tud_hid_n_ready(0) || tud_suspended();
}

extern "C" bool usb_is_suspended(void) {
    return tud_suspended();
}"""),
    ("        tud_task();",
     """        tud_task();
        // USB が列挙されないまま 3 秒経過したら D+ を落として再接続を促す (最大 5 回)
        {
            static uint64_t usb_retry_next_us = 0;
            static uint64_t usb_reconnect_at_us = 0;
            static int usb_retry_count = 0;
            uint64_t now_usb = time_us_64();
            if (tud_mounted()) {
                usb_retry_count = 0;
            } else if (usb_reconnect_at_us == 0 && usb_retry_count < 5 &&
                       now_usb >= usb_retry_next_us) {
                usb_retry_next_us = now_usb + 3000000ULL;
                usb_retry_count++;
                tud_disconnect();
                usb_reconnect_at_us = now_usb + 100000ULL;
                printf("[USB] not mounted -> re-attach (%d/5)\\r\\n", usb_retry_count);
            }
            if (usb_reconnect_at_us != 0 && now_usb >= usb_reconnect_at_us) {
                usb_reconnect_at_us = 0;
                tud_connect();
            }
        }"""),
])

# --- oled.cc ----------------------------------------------------------------
patch(WT + '/firmware/src/oled.cc', [
    ('snprintf(line0, sizeof(line0), "%s\\x86.USB-OUT          ", tx_icon);',
     'snprintf(line0, sizeof(line0), "%s\\x86%cUSB-OUT         ", tx_icon, hb_char);'),
])

# --- remote_log.cc ----------------------------------------------------------
patch(WT + '/firmware/src/remote_log.cc', [
    ("        const uint32_t lmax_ms = loop_gap_max_us / 1000u;",
     """        const uint32_t lmax_ms = loop_gap_max_us / 1000u;
#if defined(ENABLE_BLE) && ENABLE_BLE
        const bool usb_mounted = usb_is_mounted();
        const bool usb_ready = usb_is_ready();
        const bool usb_susp = usb_is_suspended();
#else
        const bool usb_mounted = false;
        const bool usb_ready = false;
        const bool usb_susp = false;
#endif"""),


])

# 複数箇所に効かせたい置換 (HB 2 種 + STALL)
p = WT + '/firmware/src/remote_log.cc'
s = open(p).read()
s = s.replace('blk=%u acl=%u disc=%u', 'blk=%u acl=%u cp=%u/%u disc=%u')
s = s.replace('rtt=%ums rmax=%ums",', 'rtt=%ums rmax=%ums u=%u/%u sus=%u",')
s = s.replace('(unsigned) h.acl_full, (unsigned) h.disc,',
              '(unsigned) h.acl_full, (unsigned) h.cp_events, (unsigned) h.cp_packets, (unsigned) h.disc,')
s = s.replace('(unsigned) lmax_ms, (unsigned) rtt_ms, (unsigned) rmax_ms);',
              '(unsigned) lmax_ms, (unsigned) rtt_ms, (unsigned) rmax_ms,\n'
              '                              (unsigned) usb_mounted, (unsigned) usb_ready, (unsigned) usb_susp);')
open(p, 'w').write(s)

# --- oled.cc: I2C エラー時の再初期化 --------------------------------------
patch(WT + '/firmware/src/oled.cc', [
    ("""static bool ssd1306_cmd(uint8_t cmd) {
    uint8_t buf[2] = {0x00, cmd};
    int res = i2c_write_timeout_us(I2C_PORT, SSD1306_I2C_ADDR, buf, 2, false, I2C_TIMEOUT_US);
    return res == 2;
}""",
     """static volatile bool oled_bus_error = false;

static bool ssd1306_cmd(uint8_t cmd) {
    uint8_t buf[2] = {0x00, cmd};
    int res = i2c_write_timeout_us(I2C_PORT, SSD1306_I2C_ADDR, buf, 2, false, I2C_TIMEOUT_US);
    if (res != 2) {
        oled_bus_error = true;
        return false;
    }
    return true;
}"""),
    ("""        int res = i2c_write_timeout_us(I2C_PORT, SSD1306_I2C_ADDR, buf, chunk + 1, false, I2C_TIMEOUT_US);
        if (res != (int)(chunk + 1)) {
            return false;
        }""",
     """        int res = i2c_write_timeout_us(I2C_PORT, SSD1306_I2C_ADDR, buf, chunk + 1, false, I2C_TIMEOUT_US);
        if (res != (int)(chunk + 1)) {
            oled_bus_error = true;
            return false;
        }"""),
    ("void oled_init(void) {", "static void ssd1306_apply_init(void);\n\nvoid oled_init(void) {"),
    ("    oled_present = true;\n\n    ssd1306_cmd(0x20); ssd1306_cmd(0x00); // Horizontal addressing mode",
     "    oled_present = true;\n\n    ssd1306_apply_init();\n}\n\nstatic void ssd1306_apply_init(void) {\n    ssd1306_cmd(0xAE);\n    ssd1306_cmd(0x20); ssd1306_cmd(0x00); // Horizontal addressing mode"),
    ("    need_redraw = false;\n    last_refresh_us = now_us;",
     """    static uint64_t last_reinit_us = 0;
    if (oled_bus_error && (now_us - last_reinit_us) > 1000000ULL) {
        oled_bus_error = false;
        last_reinit_us = now_us;
        ssd1306_apply_init();
        need_redraw = true;
        printf("[OLED] I2C error -> re-init\\r\\n");
    }

    need_redraw = false;
    last_refresh_us = now_us;"""),
])

# --- 追加修正: 全離下 / 休眠表示 ------------------------------------------
patch(WT + '/firmware/src/ble_hid.cc', [
    ("""static void send_all_release_reports(void) {
    // USB 側にキーボード全離下を送信
    usb_clear_kbd_report();

    // BLE 側にもキーボード全離下を送信
    if (ble_hid_is_connected()) {
        uint8_t zero_kbd[16] = {0};
        ble_hid_send_report(2, zero_kbd, sizeof(zero_kbd));
    }
}""",
     """static void send_all_release_reports(void) {
    usb_clear_kbd_report();
    if (ble_hid_is_connected()) {
        uint8_t zero_kbd[16] = {0};
        uint8_t zero_mouse[9] = {0};
        uint8_t zero_consumer[1] = {0};
        ble_hid_send_report(2, zero_kbd, sizeof(zero_kbd));
        ble_hid_send_report(1, zero_mouse, sizeof(zero_mouse));
        ble_hid_send_report(3, zero_consumer, sizeof(zero_consumer));
    }
}"""),
    ('        printf("[BT] Connection stabilized -> CONNECTED (Ready to send)\\r\\n");',
     """        printf("[BT] Connection stabilized -> CONNECTED (Ready to send)\\r\\n");
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
        }"""),
    ("        remote_log_eventf(\"RECONN-TO radio-off (dormant)\");\n        adv_set(0);\n        dormant_retry = true;",
     "        remote_log_eventf(\"RECONN-TO radio-off (dormant)\");\n        adv_set(0);\n        dormant_retry = true;\n        oled_set_radio_off(true);"),
    ("    btstack_run_loop_remove_timer(&reconnect_timeout_timer);\n    ble_lock();\n    current_state = BLE_STATE_RECONNECTING;",
     "    btstack_run_loop_remove_timer(&reconnect_timeout_timer);\n    oled_set_radio_off(false);\n    ble_lock();\n    current_state = BLE_STATE_RECONNECTING;"),
])

# --- OLED: 宛先未解決なら "?" --------------------------------------------
patch(WT + '/firmware/src/remote_log.cc', [
    ("""            if (rl_send_errors != 0) {
                snprintf(wi_str, sizeof(wi_str), "!%02u", (unsigned) (rl_send_errors % 100));
            } else {
                snprintf(wi_str, sizeof(wi_str), "*");
            }""",
     """            if (rl_send_errors != 0) {
                snprintf(wi_str, sizeof(wi_str), "!%02u", (unsigned) (rl_send_errors % 100));
            } else if (rl_target_arp_state == 2) {
                snprintf(wi_str, sizeof(wi_str), "?");
            } else {
                snprintf(wi_str, sizeof(wi_str), "*");
            }"""),
])

# --- Wi-Fi NONET/FAIL の短いバックオフ --------------------------------------
patch(WT + '/firmware/src/remote_log.cc', [
    ("static uint64_t connect_started_us = 0;",
     "static uint64_t connect_started_us = 0;\n"
     "// NONET/FAIL (一過性) の短いバックオフ用カウンタ。JOINED で 0 に戻す\n"
     "static uint8_t rl_quick_retry = 0;"),
    ("            if (link == CYW43_LINK_UP) {\n                wifi_state = WIFI_JOINED;",
     "            if (link == CYW43_LINK_UP) {\n                wifi_state = WIFI_JOINED;\n"
     "                rl_quick_retry = 0;"),
    ("""                reconnect_count++; // BADAUTH (-3) 等の失敗も再接続系として数える
                char code[16];""",
     """                reconnect_count++; // BADAUTH (-3) 等の失敗も再接続系として数える
                uint32_t retry_ms = RLOG_CONNECT_RETRY_MS;
                if (link == CYW43_LINK_NONET || link == CYW43_LINK_FAIL) {
                    const uint32_t shift = (rl_quick_retry < 4) ? rl_quick_retry : 4;
                    retry_ms = 2000u << shift;
                    if (rl_quick_retry < 4) rl_quick_retry++;
                } else {
                    rl_quick_retry = 0;
                }
                char code[16];"""),
    ("""                remote_log_eventf("WIFI FAIL t=%us st=%s rc=%u (retry in %ds)", rlog_upt_s(), code,
                                  (unsigned) reconnect_count, RLOG_CONNECT_RETRY_MS / 1000);""",
     """                remote_log_eventf("WIFI FAIL t=%us st=%s rc=%u (retry in %us)", rlog_upt_s(), code,
                                  (unsigned) reconnect_count, (unsigned) (retry_ms / 1000));"""),
    ("                next_connect_attempt_us = now_us + RLOG_CONNECT_RETRY_MS * 1000ULL;\n                udp_close_cleanup();",
     "                next_connect_attempt_us = now_us + (uint64_t) retry_ms * 1000ULL;\n                udp_close_cleanup();"),
])

# --- 入力再開マーカー / STALL 閾値 2 秒 ------------------------------------
patch(WT + '/firmware/src/ble_hid.cc', [
    ("""    if (status == ERROR_CODE_SUCCESS) {
        health_ok++;
        health_last_ok_us = time_us_64();""",
     """    if (status == ERROR_CODE_SUCCESS) {
        // 「放置 -> 触った」遷移のマーカー (直後に DISC が出れば引き金が再開だと分かる)
        static uint64_t last_send_us = 0;
        const uint64_t now_us = time_us_64();
        if (last_send_us != 0 && (now_us - last_send_us) >= 10000000ULL) {
            remote_log_eventf("BT RESUME gap=%us", (unsigned) ((now_us - last_send_us) / 1000000ULL));
        }
        last_send_us = now_us;
        health_ok++;
        health_last_ok_us = now_us;"""),
])
patch(WT + '/firmware/src/remote_log.cc', [
    ("#define RLOG_BLE_STALL_MS 5000 // 通知がこれだけ途絶えたら「停止」とみなす",
     "#define RLOG_BLE_STALL_MS 2000 // 短い止まりも拾う (入力が無ければ判定は取り下げ)"),
])

# --- OLED: 向き設定の再表明 ------------------------------------------------
REASSERT = """// 向き・表示設定の再表明。0xC8 (COM 走査方向) を I2C で取りこぼすと
// 上下反転のまま固まる症状が出たため、1 秒ごとに送り直して自己修復させる
static void ssd1306_reassert_settings(void) {
    ssd1306_cmd(0x20); ssd1306_cmd(0x00);
    ssd1306_cmd(0xA8); ssd1306_cmd(SSD1306_HEIGHT - 1);
    ssd1306_cmd(0xD3); ssd1306_cmd(0x00);
    ssd1306_cmd(0x40);
    ssd1306_cmd(0xA1);
    ssd1306_cmd(0xC8);
#if SSD1306_HEIGHT == 32
    ssd1306_cmd(0xDA); ssd1306_cmd(0x02);
#else
    ssd1306_cmd(0xDA); ssd1306_cmd(0x12);
#endif
    ssd1306_cmd(0x8D); ssd1306_cmd(0x14);
    ssd1306_cmd(0xA4);
    ssd1306_cmd(0xA6);
    ssd1306_cmd(0x2E);
}

void oled_set_mode(bool ble_mode) {"""
patch(WT + '/firmware/src/oled.cc', [
    ("void oled_set_mode(bool ble_mode) {", REASSERT),
    ("""    need_redraw = false;
    last_refresh_us = now_us;""",
     """    need_redraw = false;
    last_refresh_us = now_us;

    static uint64_t last_reassert_us = 0;
    if (now_us - last_reassert_us >= 1000000ULL) {
        last_reassert_us = now_us;
        ssd1306_reassert_settings();
    }"""),
])

# --- OLED: 再初期化回数のログ --------------------------------------------
patch(WT + '/firmware/src/oled.cc', [
    ("#include \"ble_hid.h\"", "#include \"ble_hid.h\"\n#include \"remote_log.h\""),
    ("static volatile bool oled_bus_error = false;",
     "static volatile bool oled_bus_error = false;\nstatic uint32_t oled_reinit_count = 0;"),
    ("""        ssd1306_apply_init();
        need_redraw = true;
        printf("[OLED] I2C error -> re-init\\r\\n");""",
     """        ssd1306_apply_init();
        need_redraw = true;
        oled_reinit_count++;
        printf("[OLED] I2C error -> re-init (#%u)\\r\\n", (unsigned) oled_reinit_count);
        remote_log_eventf("OLED REINIT n=%u", (unsigned) oled_reinit_count);"""),
])

# --- STALL: 未接続中は判定しない ------------------------------------------
patch(WT + '/firmware/src/remote_log.cc', [
    ("""    if (ok_moved) {
        ble_stall_since_us = 0;
    } else if (in_moved && ble_stall_since_us == 0) {""",
     """    if (ok_moved) {
        ble_stall_since_us = 0;
    } else if (!connected) {
        // 未接続中の入力は USB 側に流れるのが正常 (BT-OFF 中の USB 使用など)。
        // ここでタイマーを回すと再接続の瞬間に誤検知する
        ble_stall_since_us = 0;
        ble_stall_active = false;
    } else if (in_moved && ble_stall_since_us == 0) {"""),
])

# --- STALL: 未処理 3 件以上を条件に ----------------------------------------
patch(WT + '/firmware/src/remote_log.cc', [
    ("""    const bool stalled = connected && ble_stall_since_us != 0 &&
                         (now_us - ble_stall_since_us) >= (uint64_t) RLOG_BLE_STALL_MS * 1000ULL;""",
     """    const bool stalled = connected && ble_stall_since_us != 0 &&
                         (now_us - ble_stall_since_us) >= (uint64_t) RLOG_BLE_STALL_MS * 1000ULL &&
                         (in_now - ble_stall_in_start) >= 3;"""),
])

# --- OLED: 10fps 描画 ------------------------------------------------------
patch(WT + '/firmware/src/oled.cc', [
    ("""    // 約 25 FPS (40ms) または 状態変更時に描画
    if (!need_redraw && (now_us - last_refresh_us < 40000)) {""",
     """    // 約 10 FPS (100ms) または 状態変更時に描画 (バス負荷を下げる)
    if (!need_redraw && (now_us - last_refresh_us < 100000)) {"""),
])

# --- omax / gmax の区間外ギャップ除外 -------------------------------------
patch(WT + '/firmware/src/remote_log.cc', [
    ("static uint64_t last_ingest_us = 0;",
     "static uint64_t last_ingest_us = 0;\n"
     "static uint64_t ingest_window_start_us = 0;\n"
     "static uint32_t tx_max_ms = 0;"),
    ("""    if (last_ingest_us != 0) {
        uint32_t gap = (uint32_t) (now - last_ingest_us);""",
     """    if (last_ingest_us != 0 && last_ingest_us >= ingest_window_start_us) {
        uint32_t gap = (uint32_t) (now - last_ingest_us);"""),
    ("""    if (in_moved) {
        ble_last_in_us = now_us;
    }""",
     """    if (in_moved) {
        ble_last_in_us = now_us;
        if (connected && ever && tx_ms != 0xFFFFFFFFu && tx_ms > tx_max_ms) {
            tx_max_ms = tx_ms;
        }
    }"""),
    ("tx=%ums in=%u gmax=%ums", "tx=%ums omax=%ums in=%u gmax=%ums"),
    ("""                              (unsigned) h.disc_reason, (unsigned) tx_ms,
                              (unsigned) in_delta, (unsigned) gmax_ms, (unsigned) lps,""",
     """                              (unsigned) h.disc_reason, (unsigned) tx_ms,
                              (unsigned) omax_ms,
                              (unsigned) in_delta, (unsigned) gmax_ms, (unsigned) lps,"""),
    ("        ble_hb_in = count_ingest;",
     "        const uint32_t omax_ms = tx_max_ms;\n"
     "        tx_max_ms = 0;\n"
     "        ingest_window_start_us = now_us;\n"
     "        ingest_gap_max_us = 0;\n"
     "        ble_hb_in = count_ingest;"),
])

# --- burst (詰め込み送信) / lat (送信許可待ち) の計装 ----------------------
patch(WT + '/firmware/src/ble_hid.h', [
    ("    uint32_t disc;       // 切断イベントの累計",
     "    uint32_t disc;       // 切断イベントの累計\n"
     "    uint32_t burst;      // 2ms 未満で連続送信した回数 (詰め込み指標)\n"
     "    uint32_t lat_max_ms; // 送信許可要求 -> 許可までの最大待ち (ms)"),
    ("bool ble_hid_is_pairing(void);",
     "bool ble_hid_is_pairing(void);\nvoid ble_hid_reset_lat_max(void);"),
])
patch(WT + '/firmware/src/ble_hid.cc', [
    ("static volatile uint32_t health_blocked = 0;",
     "static volatile uint32_t health_blocked = 0;\n"
     "static volatile uint32_t health_burst = 0;\n"
     "static volatile uint64_t can_send_req_us = 0;\n"
     "static volatile uint32_t health_lat_max_ms = 0;\n\n"
     "void ble_hid_reset_lat_max(void) { health_lat_max_ms = 0; }"),
    ("    out->disc = health_disc;",
     "    out->burst = health_burst;\n"
     "    out->lat_max_ms = health_lat_max_ms;\n"
     "    out->disc = health_disc;"),
    ("""        if (last_send_us != 0 && (now_us - last_send_us) >= 10000000ULL) {""",
     """        if (last_send_us != 0 && (now_us - last_send_us) < 2000ULL) {
            health_burst++;
        }
        if (last_send_us != 0 && (now_us - last_send_us) >= 10000000ULL) {"""),
    ("""        uint8_t req_err = hids_device_request_can_send_now_event(con_handle);
        if (req_err != ERROR_CODE_SUCCESS) {
            ble_can_send = true;
            btstack_run_loop_remove_timer(&can_send_timeout_timer);
        }""",
     """        can_send_req_us = time_us_64();
        uint8_t req_err = hids_device_request_can_send_now_event(con_handle);
        if (req_err != ERROR_CODE_SUCCESS) {
            can_send_req_us = 0;
            ble_can_send = true;
            btstack_run_loop_remove_timer(&can_send_timeout_timer);
        }"""),
    ("""                case HIDS_SUBEVENT_CAN_SEND_NOW: {
                    consecutive_can_send_timeouts = 0;
                    ble_can_send = true;""",
     """                case HIDS_SUBEVENT_CAN_SEND_NOW: {
                    consecutive_can_send_timeouts = 0;
                    ble_can_send = true;
                    if (can_send_req_us != 0) {
                        uint32_t d = (uint32_t) ((time_us_64() - can_send_req_us) / 1000ULL);
                        if (d > health_lat_max_ms) health_lat_max_ms = d;
                        can_send_req_us = 0;
                    }"""),
])
patch(WT + '/firmware/src/remote_log.cc', [
    ("static uint32_t ble_hb_in = 0;",
     "static uint32_t ble_hb_in = 0;\nstatic uint32_t ble_hb_burst = 0;"),
    ("        const uint32_t in_delta = count_ingest - ble_hb_in;",
     "        const uint32_t in_delta = count_ingest - ble_hb_in;\n"
     "        const uint32_t burst_delta = h.burst - ble_hb_burst;"),
    ("        const uint32_t omax_ms = tx_max_ms;",
     "        const uint32_t omax_ms = tx_max_ms;\n"
     "        const uint32_t lat_ms = h.lat_max_ms;"),
    ("        ble_hb_in = count_ingest;",
     "        ble_hb_in = count_ingest;\n"
     "        ble_hb_burst = h.burst;\n"
     "        ble_hid_reset_lat_max();"),
    ("tx=%ums omax=%ums in=%u gmax=%ums", "tx=%ums omax=%ums lat=%ums burst=%u in=%u gmax=%ums"),
    ("""                              (unsigned) omax_ms,""",
     """                              (unsigned) omax_ms,
                              (unsigned) lat_ms,
                              (unsigned) burst_delta,"""),
])

# --- スパイクを時刻付きで記録 (IHGAP / SENDGAP / LATGAP) -------------------
patch(WT + '/firmware/src/remote_log.cc', [
    ("static uint32_t tx_max_ms = 0;",
     "static uint32_t tx_max_ms = 0;\n"
     "static uint64_t last_ihgap_us = 0;\n"
     "static uint64_t last_shgap_us = 0;\n"
     "static bool ble_recent_input = false;"),
    ("""        if (gap > ingest_gap_max_us) {
            ingest_gap_max_us = gap;
        }""",
     """        if (gap > ingest_gap_max_us) {
            ingest_gap_max_us = gap;
        }
        if (gap >= 100000u && ble_recent_input && (now - last_ihgap_us) >= 1000000ULL) {
            last_ihgap_us = now;
            remote_log_eventf("BT IHGAP ms=%u", (unsigned) (gap / 1000u));
        }"""),
    ("""    if (in_moved) {
        ble_last_in_us = now_us;
        if (connected && ever && tx_ms != 0xFFFFFFFFu && tx_ms > tx_max_ms) {
            tx_max_ms = tx_ms;
        }
    }""",
     """    if (in_moved) {
        ble_recent_input = true;
        ble_last_in_us = now_us;
        if (connected && ever && tx_ms != 0xFFFFFFFFu && tx_ms > tx_max_ms) {
            tx_max_ms = tx_ms;
        }
        if (connected && ever && tx_ms >= 100u && (now_us - last_shgap_us) >= 1000000ULL) {
            last_shgap_us = now_us;
            remote_log_eventf("BT SENDGAP ms=%u", (unsigned) tx_ms);
        }
    }"""),
    ("""    if (ble_stall_since_us != 0 &&
        (now_us - ble_last_in_us) > (uint64_t) RLOG_BLE_STALL_MS * 1000ULL) {""",
     """    if (ble_last_in_us != 0 && (now_us - ble_last_in_us) > 2000000ULL) {
        ble_recent_input = false;
    }
    if (ble_stall_since_us != 0 &&
        (now_us - ble_last_in_us) > (uint64_t) RLOG_BLE_STALL_MS * 1000ULL) {"""),
])
patch(WT + '/firmware/src/ble_hid.cc', [
    ("""                    if (can_send_req_us != 0) {
                        uint32_t d = (uint32_t) ((time_us_64() - can_send_req_us) / 1000ULL);
                        if (d > health_lat_max_ms) health_lat_max_ms = d;
                        can_send_req_us = 0;
                    }""",
     """                    if (can_send_req_us != 0) {
                        uint32_t d = (uint32_t) ((time_us_64() - can_send_req_us) / 1000ULL);
                        if (d > health_lat_max_ms) health_lat_max_ms = d;
                        can_send_req_us = 0;
                        if (d >= 50u) {
                            static uint64_t last_latgap_us = 0;
                            uint64_t t = time_us_64();
                            if ((t - last_latgap_us) >= 1000000ULL) {
                                last_latgap_us = t;
                                remote_log_eventf("BT LATGAP ms=%u", (unsigned) d);
                            }
                        }
                    }"""),
])

# --- スパイクは連続操作中だけ -------------------------------------------------
patch(WT + '/firmware/src/remote_log.cc', [
    ("static bool ble_recent_input = false;",
     "static bool ble_recent_input = false;\n"
     "static uint32_t in_per_sec_mark = 0;\n"
     "static uint64_t dense_until_us = 0;"),
    ("        if (gap >= 100000u && ble_recent_input && (now - last_ihgap_us) >= 1000000ULL) {",
     "        if (gap >= 100000u && now < dense_until_us && (now - last_ihgap_us) >= 1000000ULL) {"),
    ("        if (connected && ever && tx_ms >= 100u && (now_us - last_shgap_us) >= 1000000ULL) {",
     "        if (connected && ever && tx_ms >= 100u && now_us < dense_until_us && (now_us - last_shgap_us) >= 1000000ULL) {"),
    ("""    if (in_moved) {
        ble_recent_input = true;""",
     """    {
        const uint32_t in_sec = in_now - in_per_sec_mark;
        in_per_sec_mark = in_now;
        if (in_sec >= 10) {
            dense_until_us = now_us + 1500000ULL;
        }
    }
    if (in_moved) {
        ble_recent_input = true;"""),
])

# --- マウス入力だけを数える (キーボードの打鍵間隔を穴と誤認しない) -----------
patch(WT + '/firmware/src/remapper.cc', [
    ("uint8_t or_items = 0;",
     "uint8_t or_items = 0;\nvolatile uint8_t last_ingest_protocol = 0;"),
])
patch(WT + '/firmware/src/remapper.h', [
    ("void read_report(bool* new_report, bool* tick);",
     "void read_report(bool* new_report, bool* tick);\nextern volatile uint8_t last_ingest_protocol;"),
])
patch(WT + '/firmware/src/remapper_single.cc', [
    ("""        reports_received = true;""",
     """        reports_received = true;
        last_ingest_protocol = tuh_hid_interface_protocol(dev_addr, instance);"""),
])
patch(WT + '/firmware/src/remote_log.cc', [
    ('#include "remote_log.h"', '#include "remote_log.h"\n#include "remapper.h"'),
    ("static uint32_t ingest_gap_max_us = 0;",
     "static uint32_t ingest_gap_max_us = 0;\n"
     "static uint64_t last_mouse_ingest_us = 0;\n"
     "static uint32_t mouse_gap_max_us = 0;\n"
     "static uint32_t mouse_ingest_count = 0;"),
    ("""        if (gap >= 100000u && now < dense_until_us && (now - last_ihgap_us) >= 1000000ULL) {
            last_ihgap_us = now;
            remote_log_eventf("BT IHGAP ms=%u", (unsigned) (gap / 1000u));
        }
    }""",
     """    }
    if (last_ingest_protocol == 2 && last_mouse_ingest_us != 0) {
        uint32_t mgap = (uint32_t) (now - last_mouse_ingest_us);
        if (mgap > mouse_gap_max_us) {
            mouse_gap_max_us = mgap;
        }
        if (mgap >= 100000u && now < dense_until_us && (now - last_ihgap_us) >= 1000000ULL) {
            last_ihgap_us = now;
            remote_log_eventf("BT IHGAP ms=%u", (unsigned) (mgap / 1000u));
        }
    }
    if (last_ingest_protocol == 2) {
        last_mouse_ingest_us = now;
        mouse_ingest_count++;
    }"""),
    ("        const uint32_t gmax_ms = (in_delta != 0) ? (ingest_gap_max_us / 1000u) : 0;",
     "        const uint32_t gmax_ms = mouse_gap_max_us / 1000u;"),
    ("        const uint32_t in_sec = in_now - in_per_sec_mark;\n        in_per_sec_mark = in_now;",
     "        const uint32_t in_sec = mouse_ingest_count - in_per_sec_mark;\n        in_per_sec_mark = mouse_ingest_count;"),
    ("        ingest_gap_max_us = 0;\n        loop_gap_max_us = 0;",
     "        ingest_gap_max_us = 0;\n        mouse_gap_max_us = 0;\n        loop_gap_max_us = 0;"),
])

# --- マウス判定をデスクリプタ由来にする + 放置後の空白を穴に数えない ---------
patch(WT + '/firmware/src/globals.h', [
    ("extern std::unordered_map<uint16_t, bool> has_report_id_theirs;  // dev_addr+interface -> bool",
     "extern std::unordered_map<uint16_t, bool> has_report_id_theirs;  // dev_addr+interface -> bool\n"
     "extern std::unordered_map<uint16_t, bool> itf_is_pointer_theirs;"),
])
patch(WT + '/firmware/src/globals.cc', [
    ("std::unordered_map<uint16_t, bool> has_report_id_theirs;",
     "std::unordered_map<uint16_t, bool> has_report_id_theirs;\n"
     "std::unordered_map<uint16_t, bool> itf_is_pointer_theirs;"),
])
patch(WT + '/firmware/src/descriptor_parser.cc', [
    ('#include "remapper.h"', '#include "remapper.h"\n#include "remote_log.h"'),
    ("    add_synthetic_dpad_usages(their_usages[interface]);\n",
     "    add_synthetic_dpad_usages(their_usages[interface]);\n"
     "\n"
     "    {\n"
     "        bool pointer = false;\n"
     "        for (auto const& [report_id, usage_map] : their_usages[interface]) {\n"
     "            if (usage_map.count(0x00010030u) != 0) {\n"
     "                pointer = true;\n"
     "                break;\n"
     "            }\n"
     "        }\n"
     "        itf_is_pointer_theirs[interface] = pointer;\n"
     "        remote_log_eventf(\"ITF if=%04X ptr=%u\", interface, pointer ? 1 : 0);\n"
     "    }\n"),
])
patch(WT + '/firmware/src/remapper_single.cc', [
    ('#include "descriptor_parser.h"', '#include "descriptor_parser.h"\n#include "globals.h"'),
    ("        last_ingest_protocol = tuh_hid_interface_protocol(dev_addr, instance);",
     "        {\n"
     "            uint16_t itf = (uint16_t) ((dev_addr << 8) | instance);\n"
     "            auto it = itf_is_pointer_theirs.find(itf);\n"
     "            bool ptr = (it != itf_is_pointer_theirs.end()) && it->second;\n"
     "            last_ingest_protocol = (ptr || tuh_hid_interface_protocol(dev_addr, instance) == 2) ? 2 : 1;\n"
     "        }"),
])
patch(WT + '/firmware/src/remote_log.cc', [
    ("        if (mgap > mouse_gap_max_us) {\n            mouse_gap_max_us = mgap;\n        }",
     "        if (mgap < 5000000u && mgap > mouse_gap_max_us) {\n            mouse_gap_max_us = mgap;\n        }"),
    ("if (mgap >= 100000u && now < dense_until_us",
     "if (mgap >= 100000u && mgap < 5000000u && now < dense_until_us"),
])

# --- 送信が黙って捨てられた理由 / トランスポート切替 / 大きいデルタ ------------
patch(WT + '/firmware/src/ble_hid.h', [
    ("    uint8_t disc_reason;",
     "    uint32_t sk_state;\n"
     "    uint32_t sk_enc;\n"
     "    uint32_t sk_ready;\n"
     "    uint8_t flags;\n"
     "    uint8_t disc_reason;"),
])
patch(WT + '/firmware/src/ble_hid.cc', [
    ("static volatile uint32_t health_blocked = 0;",
     "static volatile uint32_t health_blocked = 0;\n"
     "static volatile uint32_t health_skip_state = 0;\n"
     "static volatile uint32_t health_skip_enc = 0;\n"
     "static volatile uint32_t health_skip_ready = 0;"),
    ("        return;\n    }\n\n    // 【BL-15】",
     "        health_skip_state++;\n        return;\n    }\n\n    // 【BL-15】"),
    ("    if (!ble_encrypted) return;",
     "    if (!ble_encrypted) {\n        health_skip_enc++;\n        return;\n    }"),
    ("    if (report_id == 2 && !ble_kbd_ready) return;",
     "    if (report_id == 2 && !ble_kbd_ready) {\n        health_skip_ready++;\n        return;\n    }"),
    ("    if (report_id == 1 && !ble_mouse_ready) return;",
     "    if (report_id == 1 && !ble_mouse_ready) {\n        health_skip_ready++;\n        return;\n    }"),
    ("    if (report_id == 3 && !ble_consumer_ready) return;",
     "    if (report_id == 3 && !ble_consumer_ready) {\n        health_skip_ready++;\n        return;\n    }"),
    ("    out->last_ok_us = health_last_ok_us;",
     "    out->last_ok_us = health_last_ok_us;\n"
     "    out->sk_state = health_skip_state;\n"
     "    out->sk_enc = health_skip_enc;\n"
     "    out->sk_ready = health_skip_ready;\n"
     "    out->flags = (uint8_t) ((current_state == BLE_STATE_CONNECTED ? 0x01 : 0) |\n"
     "                            (ble_encrypted ? 0x02 : 0) |\n"
     "                            (ble_mouse_ready ? 0x04 : 0) |\n"
     "                            (ble_kbd_ready ? 0x08 : 0) |\n"
     "                            (ble_consumer_ready ? 0x10 : 0) |\n"
     "                            (health_tx_stalled ? 0x20 : 0) |\n"
     "                            (dormant_retry ? 0x40 : 0));"),
    ('        printf("[BT] Active transport changed: %s\\r\\n", curr_ble_connected ? "BLE" : "USB");',
     '        printf("[BT] Active transport changed: %s\\r\\n", curr_ble_connected ? "BLE" : "USB");\n'
     '        remote_log_eventf("BT XPORT ble=%u", curr_ble_connected ? 1 : 0);'),
    ("""    if (report_id == 1 && report_len >= 5) {
        last_mouse_pace_us = time_us_64();
        last_mouse_buttons = report_data[0];
    }""",
     """    if (report_id == 1 && report_len >= 5) {
        const uint64_t prev_pace_us = last_mouse_pace_us;
        last_mouse_pace_us = time_us_64();
        last_mouse_buttons = report_data[0];
        static uint64_t last_bigdx_us = 0;
        int32_t dx = (int16_t) (report_data[1] | (report_data[2] << 8));
        int32_t dy = (int16_t) (report_data[3] | (report_data[4] << 8));
        const uint64_t now_us = time_us_64();
        if ((dx >= 40 || dx <= -40 || dy >= 40 || dy <= -40) &&
            prev_pace_us != 0 && (now_us - prev_pace_us) >= 100000ULL &&
            (now_us - last_bigdx_us) >= 1000000ULL) {
            last_bigdx_us = now_us;
            remote_log_eventf("BT BIGDX dx=%d dy=%d", (int) dx, (int) dy);
        }
    }"""),
])
patch(WT + '/firmware/src/remote_log.cc', [
    ('lat=%ums burst=%u in=%u gmax=%ums lps=%u',
     'lat=%ums burst=%u in=%u gmax=%ums sk=%u/%u/%u fl=0x%02X lps=%u'),
    ("                              (unsigned) burst_delta,\n                              (unsigned) in_delta, (unsigned) gmax_ms, (unsigned) lps,",
     "                              (unsigned) burst_delta,\n"
     "                              (unsigned) in_delta, (unsigned) gmax_ms,\n"
     "                              (unsigned) h.sk_state, (unsigned) h.sk_enc, (unsigned) h.sk_ready,\n"
     "                              (unsigned) h.flags, (unsigned) lps,"),
])

patch(WT + '/firmware/src/remote_log.cc', [
    ("        if (connected && ever && tx_ms != 0xFFFFFFFFu && tx_ms > tx_max_ms) {",
     "        if (connected && ever && tx_ms != 0xFFFFFFFFu && tx_ms < 5000u && tx_ms > tx_max_ms) {"),
])
# --- 1 レポートあたりの最大移動量 (dmax) --------------------------------------
patch(WT + '/firmware/src/ble_hid.h', [
    ("    uint32_t lat_max_ms;",
     "    uint32_t lat_max_ms;\n    uint32_t dmax;"),
    ("void ble_hid_reset_lat_max(void);",
     "void ble_hid_reset_lat_max(void);\nvoid ble_hid_reset_dmax(void);"),
])
patch(WT + '/firmware/src/ble_hid.cc', [
    ("void ble_hid_reset_lat_max(void) { health_lat_max_ms = 0; }",
     "static volatile uint32_t health_dmax = 0;\n"
     "void ble_hid_reset_lat_max(void) { health_lat_max_ms = 0; }\n"
     "void ble_hid_reset_dmax(void) { health_dmax = 0; }"),
    ("    out->lat_max_ms = health_lat_max_ms;",
     "    out->lat_max_ms = health_lat_max_ms;\n    out->dmax = health_dmax;"),
    ("        int32_t dy = (int16_t) (report_data[3] | (report_data[4] << 8));\n        const uint64_t now_us = time_us_64();",
     "        int32_t dy = (int16_t) (report_data[3] | (report_data[4] << 8));\n"
     "        const uint64_t now_us = time_us_64();\n"
     "        {\n"
     "            uint32_t d = (uint32_t) ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy));\n"
     "            if (d > health_dmax) health_dmax = d;\n"
     "        }"),
])
patch(WT + '/firmware/src/remote_log.cc', [
    ('gmax=%ums sk=%u/%u/%u fl=0x%02X lps=%u', 'gmax=%ums dmax=%u sk=%u/%u/%u fl=0x%02X lps=%u'),
    ("                              (unsigned) in_delta, (unsigned) gmax_ms,\n                              (unsigned) h.sk_state,",
     "                              (unsigned) in_delta, (unsigned) gmax_ms, (unsigned) h.dmax,\n                              (unsigned) h.sk_state,"),
    ("        ble_hid_reset_lat_max();",
     "        ble_hid_reset_lat_max();\n        ble_hid_reset_dmax();"),
])

print("patched:", WT)
