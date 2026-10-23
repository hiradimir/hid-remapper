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
#include "ble_hid.h"
#include "oled.h"
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
#include "remote_log.h"
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
extern "C" bool usb_is_mounted(void) {
    return tud_mounted();
}

extern "C" bool usb_is_ready(void) {
    return tud_hid_n_ready(0) || tud_suspended();
}

// USB ホストがバスをサスペンドしているか (フォールバックが効かない原因の切り分け用)
extern "C" bool usb_is_suspended(void) {
    return tud_suspended();
}

extern "C" void usb_clear_kbd_report(void) {
    uint8_t zero_kbd[16] = {0};
    if (tud_mounted() && !tud_suspended()) {
        tud_hid_n_report(0, 2, zero_kbd, sizeof(zero_kbd));
    }
}
#endif

// =========================================================================
// USB 出力専用関数（有線接続時：TinyUSB へ直接送出、BLE 側の影響を一切受けない）
// =========================================================================
static bool do_send_report_usb(uint8_t interface, const uint8_t* report_with_id, uint8_t len) {
    if (ble_hid_check_hotkey(interface, report_with_id, len)) {
        return true;
    }
    uint8_t report_id = report_with_id[0];
    const uint8_t* report_data = report_with_id + 1;
    uint8_t report_len = len - 1;
    remote_log_count_forward(); // BL-39: 転送実行カウンタ

    oled_set_mode(false); // USB mode
    oled_notify_tx();

    if (tud_suspended() &&
        (our_descriptor->should_cause_wakeup != nullptr) &&
        our_descriptor->should_cause_wakeup(interface, report_with_id, len)) {
        tud_remote_wakeup();
    } else if (!tud_suspended()) {
        tud_hid_n_report(interface, report_id, report_data, report_len);
    }
    return true;
}



void gpio_pins_init() {
    gpio_valid_pins_mask = get_gpio_valid_pins_mask();
#if ENABLE_OLED
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

void __no_inline_not_in_flash_func(do_persist_config)(uint8_t* buffer) {
#if ENABLE_BLE
    multicore_lockout_start_blocking();
#endif
#if !PICO_COPY_TO_RAM
    uint32_t ints = save_and_disable_interrupts();
#endif
    flash_range_erase(CONFIG_OFFSET_IN_FLASH, PERSISTED_CONFIG_SIZE);
    flash_range_program(CONFIG_OFFSET_IN_FLASH, buffer, PERSISTED_CONFIG_SIZE);
#if !PICO_COPY_TO_RAM
    restore_interrupts(ints);
#endif
#if ENABLE_BLE
    multicore_lockout_end_blocking();
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
void __no_inline_not_in_flash_func(core1_main)(void) {
    multicore_lockout_victim_init();
#if ENABLE_OLED
    oled_init();
    uint64_t next_oled_us = time_us_64() + 200000; // 5Hz (200ms)
    while (true) {
        uint64_t now_us = time_us_64();
        if (now_us >= next_oled_us) {
            next_oled_us = now_us + 200000;
            oled_task();
        }
        tight_loop_contents();
    }
#else
    while (true) {
        __wfe(); // イベント待ちで低消費電力スリープ（フラッシュアクセス完全停止）
    }
#endif
}

int main() {
    timer_hw->dbgpause = 0; // デバッグ時のタイマ停止を解除

    my_mutexes_init();
    gpio_pins_init();               
    
    stdio_init_all();

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
    remote_log_init(); // BL-39 (ENABLE_REMOTE_LOG=0 では惰性スタブ)
    tud_sof_isr_set(sof_handler);

    printf("\r\n========================================\r\n");
#if ENABLE_BLE
    printf("   BLE-USB HID Bridge\r\n");
#else
    printf("   USB HID Remapper\r\n");
#endif
    printf("========================================\r\n");
    printf("=== REMAPPER READY ===\r\n\r\n");
    fflush(stdout);

    next_print = time_us_64() + 1000000;

    while (true) {
        remote_log_count_loop(); // BL-39: ループ速度の観測 (スタック検出用)

        bool tick;
        bool new_report;
        read_report(&new_report, &tick);
        oled_update_usb_ports();
        if (new_report) {
            activity_led_on();
            oled_notify_activity();
            remote_log_count_ingest(); // BL-39: 入力拾いカウンタ
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
        // USB が列挙されないまま数秒経過したら、D+ を一度落として再接続を促す。
        // 焼き直し直後に Mac/ハブが再列挙に失敗し、挿し直すまで USB-OUT が
        // 使えない現象の自動復帰 (最大 5 回まで、3 秒間隔)
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
                printf("[USB] not mounted -> re-attach (%d/5)\r\n", usb_retry_count);
            }
            if (usb_reconnect_at_us != 0 && now_usb >= usb_reconnect_at_us) {
                usb_reconnect_at_us = 0;
                tud_connect();
            }
        }
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
        ble_transport_task(do_send_report_usb);
#else
        if (tud_hid_n_ready(0) || tud_suspended()) {
            send_report(do_send_report_usb);
        }
#endif
        if (monitor_enabled && tud_hid_n_ready(1)) {
            send_monitor_report(do_send_report_usb);
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

        remote_log_task(); // BL-39: Wi-Fi 接続管理・UDP フラッシュ・ALIVE ピング (内部スロットリング)

        activity_led_off_maybe();


    }

    return 0;
}
