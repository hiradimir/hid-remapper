#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <bsp/board_api.h>
#include <tusb.h>
#include "hardware/structs/timer.h"

#ifdef ADC_ENABLED
#include <hardware/adc.h>
#endif
#if ENABLE_BLE
#include "ble_hid.h"
#include "oled.h"
#endif
#include <hardware/flash.h>
#include <hardware/gpio.h>
#include <pico/bootrom.h>
#include <pico/mutex.h>
#include <pico/platform.h>
#include <pico/stdio.h>
#include <pico/unique_id.h>
#include <pico/multicore.h>
#include "pico/stdio_uart.h"

#include "activity_led.h"
#include "config.h"
#include "crc.h"
#include "descriptor_parser.h"
#include "globals.h"
#include "i2c.h"
#include "mcp4651.h"
#include "our_descriptor.h"
#include "platform.h"
#include "remapper.h"
#include "tick.h"



// RP2350 UF2s wipe the last sector of flash every time
// because of RP2350-E10 errata mitigation. So we put
// the config one sector down.
#if PICO_RP2350
#define CONFIG_OFFSET_IN_FLASH (PICO_FLASH_SIZE_BYTES - PERSISTED_CONFIG_SIZE - 4096)
#else
#define CONFIG_OFFSET_IN_FLASH (PICO_FLASH_SIZE_BYTES - PERSISTED_CONFIG_SIZE)
#endif

#define FLASH_CONFIG_IN_MEMORY (((uint8_t*) XIP_BASE) + CONFIG_OFFSET_IN_FLASH)

#define ADC_USAGE_PAGE 0xFFF80000

uint64_t next_print = 0;

mutex_t mutexes[(uint8_t) MutexId::N];

uint32_t gpio_valid_pins_mask = 0;
uint32_t gpio_in_mask = 0;
uint32_t gpio_out_mask = 0;
uint32_t prev_gpio_state = 0;
uint64_t last_gpio_change[32] = { 0 };
bool set_gpio_dir_pending = false;

#ifdef ADC_ENABLED
uint16_t prev_adc_state[NADCS] = { 0 };
#endif

void print_stats_maybe() {
    uint64_t now = time_us_64();
    if (now > next_print) {
        print_stats();
        while (next_print < now) {
            next_print += 1000000;
        }
    }
}

void __no_inline_not_in_flash_func(sof_handler)(uint32_t frame_count) {
    sof_callback();
}

#if ENABLE_BLE
// 空のレポート（全キー・全ボタン離下）を送信してホスト側のキー押しっぱなし（スタック）を防止する
static void send_all_release_reports(void) {
    uint8_t zero_kbd[16] = {0};

    // USB 側にキーボード全離下を送信
    if (tud_mounted() && !tud_suspended()) {
        tud_hid_n_report(0, 2, zero_kbd, sizeof(zero_kbd));
    }

    // BLE 側にもキーボード全離下を送信
    if (ble_hid_is_connected()) {
        ble_hid_send_report(2, zero_kbd, sizeof(zero_kbd));
    }
}

// キーボードのホットキー（Win/Cmd + Ctrl + F1..F4）をチェックする関数
// ホットキーが押された場合は true を返し、OS へレポートを送信しない（消費する）
static bool check_pairing_hotkey(uint8_t interface, const uint8_t* report_with_id, uint8_t len) {
    if (len < 9) return false;

    uint8_t report_id = report_with_id[0];
    const uint8_t* report_data = report_with_id + 1;
    uint8_t report_len = len - 1;

    // キーボードレポート（ID 2）のみ対象
    if (report_id != 2) return false;

    uint8_t modifier = report_data[0];
    
    // Left/Right の Ctrl (0x01 / 0x10) と Win/GUI (0x08 / 0x80) の両方が含まれているか
    bool ctrl_pressed = (modifier & 0x11) != 0; // Left/Right Ctrl
    bool gui_pressed  = (modifier & 0x88) != 0; // Left/Right GUI (Win/Cmd)
    bool alt_pressed  = (modifier & 0x44) != 0; // Left/Right Alt

    // F1 〜 F4 のスロット判定（通常数字キーやテンキーの誤判定を完全に排除）
    int slot = -1;
    if      ((report_data[7] & 0x40) != 0) slot = 0; // F1 -> Slot 1
    else if ((report_data[7] & 0x80) != 0) slot = 1; // F2 -> Slot 2
    else if ((report_data[8] & 0x01) != 0) slot = 2; // F3 -> Slot 3
    else if ((report_data[8] & 0x02) != 0) slot = 3; // F4 -> Slot 4

    // デバッグ出力: Ctrl+Win と F1..F4 が押されたら UART に出力
    if (ctrl_pressed && gui_pressed && slot >= 0) {
        printf("[KBD-DEBUG] mod=0x%02X(count=%d) slot=%d\r\n", modifier, 2 + (alt_pressed ? 1 : 0), slot + 1);
    }

    static bool hotkey_was_pressed = false;
    bool hotkey_now = (ctrl_pressed && gui_pressed && (slot >= 0));

    if (hotkey_now) {
        if (!hotkey_was_pressed) {
            hotkey_was_pressed = true;
            // ホットキー成立直前に先行してホストに届いてしまった Ctrl/Cmd を打ち消すため全離下を送信
            send_all_release_reports();

            if (alt_pressed) {
                printf("[BT] *** HOTKEY: Enter Pairing Mode for slot %d ***\r\n", slot + 1);
                ble_enter_pairing_mode(slot);
            } else {
                printf("[BT] *** HOTKEY: Switch to slot %d ***\r\n", slot + 1);
                ble_switch_slot(slot);
            }
        }
        return true; // ホットキーは消費して OS には送信しない
    }

    hotkey_was_pressed = false;
    return false;
}
#endif

extern std::unordered_map<uint8_t, uint8_t> hub_ports;

#if ENABLE_BLE && ENABLE_OLED
// デバイスアドレスから HID 種別（KBD / MS / DEV）を判定
static const char* classify_device_type(uint8_t dev_addr) {
    bool is_kbd = false;
    bool is_mouse = false;

    // 1. their_usages (パース済みHIDディスクリプタ) から判定 (Report Protocol対応)
    for (uint8_t inst = 0; inst < 4; inst++) {
        uint16_t itf = ((uint16_t)dev_addr << 8) | inst;
        auto it = their_usages.find(itf);
        if (it != their_usages.end()) {
            for (auto const& [rep_id, usage_map] : it->second) {
                for (auto const& [usage, udef] : usage_map) {
                    uint32_t page = usage & 0xFFFF0000;
                    if (page == 0x00070000) {
                        is_kbd = true;
                    } else if (usage == 0x00010030 || usage == 0x00010031 || page == 0x00090000) {
                        is_mouse = true;
                    }
                }
            }
        }
    }
    if (is_kbd) return "KBD";
    if (is_mouse) return "MS ";

    // 2. TinyUSB インターフェースプロトコル (Boot Protocol) から判定
    for (uint8_t inst = 0; inst < 2; inst++) {
        uint8_t proto = tuh_hid_interface_protocol(dev_addr, inst);
        if (proto == HID_ITF_PROTOCOL_KEYBOARD) return "KBD";
        if (proto == HID_ITF_PROTOCOL_MOUSE) return "MS ";

        tuh_itf_info_t itf_info;
        if (tuh_hid_itf_get_info(dev_addr, inst, &itf_info)) {
            if (itf_info.desc.bInterfaceProtocol == HID_ITF_PROTOCOL_KEYBOARD) return "KBD";
            if (itf_info.desc.bInterfaceProtocol == HID_ITF_PROTOCOL_MOUSE) return "MS ";
        }
    }

    return "DEV";
}

// W-01/W-17: USB Host P1/P2 のデバイス種別（KBD/MS/DEV/---）を正確かつ安全に検出
static void update_usb_ports_status(void) {
    const char* p1_dev = "---";
    const char* p2_dev = "---";

    // 物理ポートの結線レベル検知 (GP0/1: Host 1, GP2/3: Host 2)
    bool port1_phys = (gpio_get(0) != 0) || (gpio_get(1) != 0);
    bool port2_phys = (gpio_get(2) != 0) || (gpio_get(3) != 0);

    uint8_t mounted_addrs[4];
    int mounted_count = 0;
    for (uint8_t dev_addr = 1; dev_addr <= 4; dev_addr++) {
        if (tuh_mounted(dev_addr)) {
            mounted_addrs[mounted_count++] = dev_addr;
        }
    }

    if (mounted_count == 0) {
        p1_dev = "---";
        p2_dev = "---";
    } else if (mounted_count == 1) {
        const char* dtype = classify_device_type(mounted_addrs[0]);
        if (port1_phys && !port2_phys) {
            p1_dev = dtype;
            p2_dev = "---";
        } else if (!port1_phys && port2_phys) {
            p1_dev = "---";
            p2_dev = dtype;
        } else {
            // アドレスまたはハブポートによる割当
            uint8_t hub_addr = 0, hub_port = 0;
            tuh_get_hub_addr_port(mounted_addrs[0], &hub_addr, &hub_port);
            if (hub_port == 1 || mounted_addrs[0] == 2) {
                p2_dev = dtype;
            } else {
                p1_dev = dtype;
            }
        }
    } else {
        // 2台以上接続時
        p1_dev = classify_device_type(mounted_addrs[0]);
        p2_dev = classify_device_type(mounted_addrs[1]);
        if (!port1_phys && port2_phys) {
            const char* tmp = p1_dev;
            p1_dev = "---";
            p2_dev = tmp;
        }
    }

    static char prev_p1[4] = "";
    static char prev_p2[4] = "";
    if (strncmp(prev_p1, p1_dev, 3) != 0 || strncmp(prev_p2, p2_dev, 3) != 0) {
        strncpy(prev_p1, p1_dev, sizeof(prev_p1));
        strncpy(prev_p2, p2_dev, sizeof(prev_p2));
        printf("[USB] Ports updated -> P1:%s, P2:%s (count=%d, p1_pin=%d, p2_pin=%d)\r\n", 
                       p1_dev, p2_dev, mounted_count, port1_phys, port2_phys);
    }

    oled_set_usb_port(0, p1_dev);
    oled_set_usb_port(1, p2_dev);
}
#endif

bool do_send_report(uint8_t interface, const uint8_t* report_with_id, uint8_t len) {

#if ENABLE_BLE
    uint8_t report_id = report_with_id[0];
    const uint8_t* report_data = report_with_id + 1;
    uint8_t report_len = len - 1;

    // ホットキーが押された場合は OS へ送らず破棄
    if (check_pairing_hotkey(interface, report_with_id, len)) {
        return true;
    }

    // BLE 接続状態の変化（未接続 -> 接続完了）を検知し、
    // USB 側に残っているかもしれないキーを一度完全にクリアする
    static bool prev_ble_connected = false;
    bool curr_ble_connected = ble_hid_is_connected();
    if (curr_ble_connected && !prev_ble_connected) {
        printf("[BT] Switched to BLE priority, clearing USB reports\r\n");
        uint8_t zero_kbd[16] = {0};
        if (tud_mounted() && !tud_suspended()) {
            tud_hid_n_report(0, 2, zero_kbd, sizeof(zero_kbd));
        }
    }
    prev_ble_connected = curr_ble_connected;

    oled_set_mode(curr_ble_connected);
    oled_notify_tx();

    // BLEで接続がされていたら、優先する
    if (curr_ble_connected) {
        ble_hid_send_report(report_id, report_data, report_len);
    } else if (tud_mounted()) {
        if (tud_suspended() &&
            (our_descriptor->should_cause_wakeup != nullptr) &&
            our_descriptor->should_cause_wakeup(interface, report_with_id, len)) {
            tud_remote_wakeup();
        } else if (!tud_suspended()) {
            tud_hid_n_report(interface, report_id, report_data, report_len);
        }
    }
#else
    // 有線専用ビルド
    if (tud_suspended() &&
        (our_descriptor->should_cause_wakeup != nullptr) &&
        our_descriptor->should_cause_wakeup(report_with_id[0], report_with_id + 1, len - 1)) {
        tud_remote_wakeup();
    } else {
        tud_hid_n_report(interface, report_with_id[0], report_with_id + 1, len - 1);
    }
#endif

    return true;
}

void gpio_pins_init() {
    gpio_valid_pins_mask = get_gpio_valid_pins_mask();
#if ENABLE_BLE && ENABLE_OLED
    // GP4 (I2C0 SDA) と GP5 (I2C0 SCL) を OLED 専用として GPIO マッピングから除外
    gpio_valid_pins_mask &= ~((1 << 4) | (1 << 5));
#endif
    gpio_init_mask(gpio_valid_pins_mask);
}

void set_gpio_inout_masks(uint32_t in_mask, uint32_t out_mask) {
    // if some pin appears as both input and output, input wins
    gpio_out_mask = (out_mask & ~in_mask) & gpio_valid_pins_mask;
    // we treat all pins except the output ones as input so that the monitor works
    gpio_in_mask = gpio_valid_pins_mask & ~gpio_out_mask;
    set_gpio_dir_pending = true;
}

void set_gpio_dir() {
    gpio_set_dir_masked(gpio_in_mask, 0);
    // output pin direction will be set in write_gpio()
    for (uint8_t i = 0; i <= 29; i++) {
        uint32_t bit = 1 << i;
        if (gpio_valid_pins_mask & bit) {
            gpio_set_pulls(i, gpio_in_mask & bit, false);
        }
    }
}

#ifdef ADC_ENABLED
void adc_pins_init() {
    adc_init();
    for (int n = 26; n < 26 + NADCS; n++) {
        adc_gpio_init(n);
    }

#ifdef PICO_SMPS_MODE_PIN
    // (This only does anything on a Pico, but won't hurt on custom board v8.)
    gpio_init(PICO_SMPS_MODE_PIN);
    gpio_set_dir(PICO_SMPS_MODE_PIN, GPIO_OUT);
    gpio_put(PICO_SMPS_MODE_PIN, true);
#endif
}
#endif

bool read_gpio(uint64_t now) {
    uint32_t gpio_state = gpio_get_all() & gpio_in_mask;
    uint32_t changed = prev_gpio_state ^ gpio_state;
    if (changed != 0) {
        for (uint8_t i = 0; i <= 29; i++) {
            uint32_t bit = 1 << i;
            if (changed & bit) {
                if (last_gpio_change[i] + gpio_debounce_time <= now) {
                    uint32_t usage = GPIO_USAGE_PAGE | i;
                    int32_t state = !(gpio_state & bit);  // active low
                    set_input_state(usage, state, state);
                    if (monitor_enabled) {
                        monitor_usage(usage, state, 0);
                    }
                    last_gpio_change[i] = now;
                } else {
                    // ignore this change
                    gpio_state ^= bit;
                    changed ^= bit;
                }
            }
        }
        prev_gpio_state = gpio_state;
    }
    return changed != 0;
}

void write_gpio() {
    if (suspended) {
        return;
    }

    uint32_t value = gpio_out_state[0] | (gpio_out_state[1] << 8) | (gpio_out_state[2] << 16) | (gpio_out_state[3] << 24);
    switch (gpio_output_mode) {
        case 0:
            gpio_put_masked(gpio_out_mask, value);
            gpio_set_dir_masked(gpio_out_mask, gpio_out_mask);
            break;
        case 1:
            gpio_put_masked(gpio_out_mask, 0);
            gpio_set_dir_masked(gpio_out_mask, value);
            break;
    }
    memset(gpio_out_state, 0, sizeof(gpio_out_state));
}

#ifdef ADC_ENABLED
bool read_adc() {
    bool changed = false;
    for (int i = 0; i < NADCS; i++) {
        adc_select_input(i);
        uint16_t state = adc_read();
        if (state != prev_adc_state[i]) {
            changed = true;
            prev_adc_state[i] = state;
        }
        uint32_t usage = ADC_USAGE_PAGE | i;
        set_input_state(usage, state, state >> 4);
        if (monitor_enabled) {
            monitor_usage(usage, state, 0);
        }
    }
    return changed;
}
#endif

void do_persist_config(uint8_t* buffer) {
#if !PICO_COPY_TO_RAM
    uint32_t ints = save_and_disable_interrupts();
#endif
    flash_range_erase(CONFIG_OFFSET_IN_FLASH, PERSISTED_CONFIG_SIZE);
    flash_range_program(CONFIG_OFFSET_IN_FLASH, buffer, PERSISTED_CONFIG_SIZE);
#if !PICO_COPY_TO_RAM
    restore_interrupts(ints);
#endif
}

void reset_to_bootloader() {
    reset_usb_boot(0, 0);
}

void pair_new_device() {
#if ENABLE_BLE
    ble_pair_new_device();
#endif
}

void clear_bonds() {
#if ENABLE_BLE
    ble_clear_bonds();
#endif
}

void my_mutexes_init() {
    for (int i = 0; i < (int8_t) MutexId::N; i++) {
        mutex_init(&mutexes[i]);
    }
}

void my_mutex_enter(MutexId id) {
    mutex_enter_blocking(&mutexes[(uint8_t) id]);
}

void my_mutex_exit(MutexId id) {
    mutex_exit(&mutexes[(uint8_t) id]);
}

uint64_t get_time() {
    return time_us_64();
}

uint64_t get_unique_id() {
    pico_unique_board_id_t unique_id;
    pico_get_unique_board_id(&unique_id);
    uint64_t ret = 0;
    for (int i = 0; i < 8; i++) {
        ret |= (uint64_t) unique_id.id[7 - i] << (8 * i);
    }
    return ret;
}


// Core 1 用メイン関数
// 将来の OLED 状態表示（BTスロット、接続先情報など）の描画タスクをここに配置可能
// Core 1 用メイン関数
// sleep_ms() はハードウェアタイマ割り込み(TIMER_IRQ)を伴い Core 1 で未処理IRQトラップを引き起こす危険があるため、
// time_us_64() と tight_loop_contents() による安全なノンブロッキング待機を使用
void core1_main(void) {
    multicore_lockout_victim_init();
#if ENABLE_BLE && ENABLE_OLED
    oled_init();
    uint64_t next_oled_us = time_us_64() + 50000;
    while (true) {
        uint64_t now_us = time_us_64();
        if (now_us >= next_oled_us) {
            next_oled_us = now_us + 50000;
            oled_task();
        }
        tight_loop_contents();
    }
#else
    while (true) {
        tight_loop_contents();
    }
#endif
}

int main() {
    timer_hw->dbgpause = 0; // デバッグ時のタイマ停止を解除

    my_mutexes_init();
    gpio_pins_init();               
    
    stdio_init_all();
    printf("\r\n\r\n=== REMAPPER BOOT ===\r\n");
    fflush(stdout);

    // デバッガ接続時や再起動時にも Core 1 を確実に起動させるため、
    // まずハードウェアリセットと FIFO クリアを行ってから launch する
    multicore_reset_core1();
    multicore_fifo_drain();
    multicore_launch_core1(core1_main);
#ifdef I2C_ENABLED
    our_i2c_init();
#endif
#ifdef ADC_ENABLED
    adc_pins_init();
#endif
    tick_init();
    load_config(FLASH_CONFIG_IN_MEMORY);
    our_descriptor = &our_descriptors[our_descriptor_number];
    parse_our_descriptor();
    set_mapping_from_config();
    board_init();
    extra_init();
    tusb_init();
#if ENABLE_DUAL_HOST
    extra_host_ports_init();
#endif

#if ENABLE_BLE
    ble_hid_init();
#endif    
    tud_sof_isr_set(sof_handler);
    
    next_print = time_us_64() + 1000000;

    while (true) {


        bool tick;
        bool new_report;
        read_report(&new_report, &tick);
#if ENABLE_BLE && ENABLE_OLED
        static uint32_t last_usb_check_ms = 0;
        uint32_t now_ms = (uint32_t)(time_us_64() / 1000);
        if (now_ms - last_usb_check_ms >= 50) {
            last_usb_check_ms = now_ms;
            update_usb_ports_status();
        }
#endif
        if (new_report) {
            activity_led_on();
        }
        if (their_descriptor_updated) {
            update_their_descriptor_derivates();
            their_descriptor_updated = false;
        }
        if (tick) {
            bool gpio_state_changed = read_gpio(time_us_64());
            if (gpio_state_changed) {
                activity_led_on();
            }
#ifdef ADC_ENABLED
            read_adc();
#endif
            process_mapping(true);
            write_gpio();
#ifdef MCP4651_ENABLED
            mcp4651_write();
#endif
        }
        tud_task();
        if (boot_protocol_updated) {
            parse_our_descriptor();
            boot_protocol_updated = false;
            config_updated = true;
        }
        if (resume_pending) {
            resume_pending = false;
            suspended = false;
        }
        if (config_updated) {
            set_mapping_from_config();
            config_updated = false;
        }
        if (set_gpio_dir_pending && !suspended) {
            set_gpio_dir();
            set_gpio_dir_pending = false;
        }
#if ENABLE_BLE
        bool ready_to_send = false;
        if (ble_hid_is_connected()) {
            ready_to_send = ble_hid_can_send();
        } else if (tud_mounted()) {
            ready_to_send = tud_hid_n_ready(0) || tud_suspended();
        } else {
            // BLE未接続かつUSB未接続（給電のみ、または未接続時）でも
            // ホットキー判定および内部バッファ（OR_BUFSIZE=8）の溢れ（overflow!）防止のために
            // send_report を呼び出してキューを安全に消化する
            ready_to_send = true;
        }

        if (ready_to_send) {
            send_report(do_send_report);
        }
        ble_hid_task();
#else
        if (tud_hid_n_ready(0) || tud_suspended()) {
            send_report(do_send_report);
        }
#endif
        if (monitor_enabled && tud_hid_n_ready(1)) {
            send_monitor_report(do_send_report);
        }
        if (our_descriptor->main_loop_task != nullptr) {
            our_descriptor->main_loop_task();
        }
        send_out_report();
        if (need_to_persist_config) {
            persist_config_return_code = persist_config();
            need_to_persist_config = false;
        }

        print_stats_maybe();

        activity_led_off_maybe();


    }

    return 0;
}
