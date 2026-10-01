#ifndef OLED_H
#define OLED_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OLED_BT_DISCONNECTED = 0,
    OLED_BT_CONNECTED    = 1,
    OLED_BT_PAIRING      = 2,
    OLED_BT_SWITCHING    = 3,
} oled_bt_state_t;

#if defined(ENABLE_OLED) && ENABLE_OLED

void oled_init(void);
void oled_task(void);

// 状態更新関数（スレッドセーフ：Core 0 から随時更新可能）
void oled_set_slot(uint8_t slot);
void oled_set_bt_state(oled_bt_state_t state);
void oled_set_status(const char* status_msg);
void oled_set_line1(const char* text);
void oled_set_line2(const char* text);

#else

// ENABLE_OLED=OFF 時のインラインスタブ（呼び出し側のオーバーヘッドを完全にゼロ化）
static inline void oled_init(void) {}
static inline void oled_task(void) {}
static inline void oled_set_slot(uint8_t slot) { (void)slot; }
static inline void oled_set_bt_state(oled_bt_state_t state) { (void)state; }
static inline void oled_set_status(const char* status_msg) { (void)status_msg; }
static inline void oled_set_line1(const char* text) { (void)text; }
static inline void oled_set_line2(const char* text) { (void)text; }

#endif

#ifdef __cplusplus
}
#endif

#endif
