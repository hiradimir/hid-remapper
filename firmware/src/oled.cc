#include "oled.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include <unordered_map>
#include "tusb.h"
#include "globals.h"

#define I2C_PORT i2c0
#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5
#define I2C_BAUDRATE 400000
#define SSD1306_I2C_ADDR 0x3C
#define I2C_TIMEOUT_US 5000

#define SSD1306_WIDTH 128
#define SSD1306_HEIGHT 32

#define FONT_WIDTH 6
#define FONT_FIRST_CHAR 0x20
#define FONT_LAST_CHAR 0x86
#define FONT_CHAR_COUNT (FONT_LAST_CHAR - FONT_FIRST_CHAR + 1)

const uint8_t font_data[FONT_CHAR_COUNT][FONT_WIDTH] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // ' ' (0x20)
    {0x00, 0x00, 0x5F, 0x00, 0x00, 0x00},  // '!' (0x21)
    {0x00, 0x07, 0x00, 0x07, 0x00, 0x00},  // '"' (0x22)
    {0x14, 0x7F, 0x14, 0x7F, 0x14, 0x00},  // '#' (0x23)
    {0x24, 0x2A, 0x7F, 0x2A, 0x12, 0x00},  // '$' (0x24)
    {0x23, 0x13, 0x08, 0x64, 0x62, 0x00},  // '%' (0x25)
    {0x36, 0x49, 0x55, 0x22, 0x50, 0x00},  // '&' (0x26)
    {0x00, 0x05, 0x03, 0x00, 0x00, 0x00},  // '\'' (0x27)
    {0x00, 0x1C, 0x22, 0x41, 0x00, 0x00},  // '(' (0x28)
    {0x00, 0x41, 0x22, 0x1C, 0x00, 0x00},  // ')' (0x29)
    {0x08, 0x2A, 0x1C, 0x2A, 0x08, 0x00},  // '*' (0x2A)
    {0x08, 0x08, 0x3E, 0x08, 0x08, 0x00},  // '+' (0x2B)
    {0x00, 0x50, 0x30, 0x00, 0x00, 0x00},  // ',' (0x2C)
    {0x08, 0x08, 0x08, 0x08, 0x08, 0x00},  // '-' (0x2D)
    {0x00, 0x60, 0x60, 0x00, 0x00, 0x00},  // '.' (0x2E)
    {0x20, 0x10, 0x08, 0x04, 0x02, 0x00},  // '/' (0x2F)
    {0x3E, 0x51, 0x49, 0x45, 0x3E, 0x00},  // '0' (0x30)
    {0x00, 0x42, 0x7F, 0x40, 0x00, 0x00},  // '1' (0x31)
    {0x42, 0x61, 0x51, 0x49, 0x46, 0x00},  // '2' (0x32)
    {0x21, 0x41, 0x45, 0x4B, 0x31, 0x00},  // '3' (0x33)
    {0x18, 0x14, 0x12, 0x7F, 0x10, 0x00},  // '4' (0x34)
    {0x27, 0x45, 0x45, 0x45, 0x39, 0x00},  // '5' (0x35)
    {0x3C, 0x4A, 0x49, 0x49, 0x30, 0x00},  // '6' (0x36)
    {0x01, 0x71, 0x09, 0x05, 0x03, 0x00},  // '7' (0x37)
    {0x36, 0x49, 0x49, 0x49, 0x36, 0x00},  // '8' (0x38)
    {0x06, 0x49, 0x49, 0x29, 0x1E, 0x00},  // '9' (0x39)
    {0x00, 0x36, 0x36, 0x00, 0x00, 0x00},  // ':' (0x3A)
    {0x00, 0x56, 0x36, 0x00, 0x00, 0x00},  // ';' (0x3B)
    {0x00, 0x08, 0x14, 0x22, 0x41, 0x00},  // '<' (0x3C)
    {0x14, 0x14, 0x14, 0x14, 0x14, 0x00},  // '=' (0x3D)
    {0x00, 0x41, 0x22, 0x14, 0x08, 0x00},  // '>' (0x3E)
    {0x02, 0x01, 0x51, 0x09, 0x06, 0x00},  // '?' (0x3F)
    {0x32, 0x49, 0x79, 0x41, 0x3E, 0x00},  // '@' (0x40)
    {0x7E, 0x11, 0x11, 0x11, 0x7E, 0x00},  // 'A' (0x41)
    {0x7F, 0x49, 0x49, 0x49, 0x36, 0x00},  // 'B' (0x42)
    {0x3E, 0x41, 0x41, 0x41, 0x22, 0x00},  // 'C' (0x43)
    {0x7F, 0x41, 0x41, 0x22, 0x1C, 0x00},  // 'D' (0x44)
    {0x7F, 0x49, 0x49, 0x49, 0x41, 0x00},  // 'E' (0x45)
    {0x7F, 0x09, 0x09, 0x01, 0x01, 0x00},  // 'F' (0x46)
    {0x3E, 0x41, 0x41, 0x51, 0x32, 0x00},  // 'G' (0x47)
    {0x7F, 0x08, 0x08, 0x08, 0x7F, 0x00},  // 'H' (0x48)
    {0x00, 0x41, 0x7F, 0x41, 0x00, 0x00},  // 'I' (0x49)
    {0x20, 0x40, 0x41, 0x3F, 0x01, 0x00},  // 'J' (0x4A)
    {0x7F, 0x08, 0x14, 0x22, 0x41, 0x00},  // 'K' (0x4B)
    {0x7F, 0x40, 0x40, 0x40, 0x40, 0x00},  // 'L' (0x4C)
    {0x7F, 0x02, 0x04, 0x02, 0x7F, 0x00},  // 'M' (0x4D)
    {0x7F, 0x04, 0x08, 0x10, 0x7F, 0x00},  // 'N' (0x4E)
    {0x3E, 0x41, 0x41, 0x41, 0x3E, 0x00},  // 'O' (0x4F)
    {0x7F, 0x09, 0x09, 0x09, 0x06, 0x00},  // 'P' (0x50)
    {0x3E, 0x41, 0x51, 0x21, 0x5E, 0x00},  // 'Q' (0x51)
    {0x7F, 0x09, 0x19, 0x29, 0x46, 0x00},  // 'R' (0x52)
    {0x46, 0x49, 0x49, 0x49, 0x31, 0x00},  // 'S' (0x53)
    {0x01, 0x01, 0x7F, 0x01, 0x01, 0x00},  // 'T' (0x54)
    {0x3F, 0x40, 0x40, 0x40, 0x3F, 0x00},  // 'U' (0x55)
    {0x1F, 0x20, 0x40, 0x20, 0x1F, 0x00},  // 'V' (0x56)
    {0x7F, 0x20, 0x18, 0x20, 0x7F, 0x00},  // 'W' (0x57)
    {0x63, 0x14, 0x08, 0x14, 0x63, 0x00},  // 'X' (0x58)
    {0x03, 0x04, 0x78, 0x04, 0x03, 0x00},  // 'Y' (0x59)
    {0x61, 0x51, 0x49, 0x45, 0x43, 0x00},  // 'Z' (0x5A)
    {0x00, 0x00, 0x7F, 0x41, 0x41, 0x00},  // '[' (0x5B)
    {0x02, 0x04, 0x08, 0x10, 0x20, 0x00},  // '\' (0x5C)
    {0x00, 0x41, 0x41, 0x7F, 0x00, 0x00},  // ']' (0x5D)
    {0x04, 0x02, 0x01, 0x02, 0x04, 0x00},  // '^' (0x5E)
    {0x40, 0x40, 0x40, 0x40, 0x40, 0x00},  // '_' (0x5F)
    {0x00, 0x01, 0x02, 0x04, 0x00, 0x00},  // '`' (0x60)
    {0x20, 0x54, 0x54, 0x54, 0x78, 0x00},  // 'a' (0x61)
    {0x7F, 0x48, 0x44, 0x44, 0x38, 0x00},  // 'b' (0x62)
    {0x38, 0x44, 0x44, 0x44, 0x20, 0x00},  // 'c' (0x63)
    {0x38, 0x44, 0x44, 0x48, 0x7F, 0x00},  // 'd' (0x64)
    {0x38, 0x54, 0x54, 0x54, 0x18, 0x00},  // 'e' (0x65)
    {0x08, 0x7E, 0x09, 0x01, 0x02, 0x00},  // 'f' (0x66)
    {0x08, 0x14, 0x54, 0x54, 0x3C, 0x00},  // 'g' (0x67)
    {0x7F, 0x08, 0x04, 0x04, 0x78, 0x00},  // 'h' (0x68)
    {0x00, 0x44, 0x7D, 0x40, 0x00, 0x00},  // 'i' (0x69)
    {0x20, 0x40, 0x44, 0x3D, 0x00, 0x00},  // 'j' (0x6A)
    {0x00, 0x7F, 0x10, 0x28, 0x44, 0x00},  // 'k' (0x6B)
    {0x00, 0x41, 0x7F, 0x40, 0x00, 0x00},  // 'l' (0x6C)
    {0x7C, 0x04, 0x18, 0x04, 0x78, 0x00},  // 'm' (0x6D)
    {0x7C, 0x08, 0x04, 0x04, 0x78, 0x00},  // 'n' (0x6E)
    {0x38, 0x44, 0x44, 0x44, 0x38, 0x00},  // 'o' (0x6F)
    {0x7C, 0x14, 0x14, 0x14, 0x08, 0x00},  // 'p' (0x70)
    {0x08, 0x14, 0x14, 0x18, 0x7C, 0x00},  // 'q' (0x71)
    {0x7C, 0x08, 0x04, 0x04, 0x08, 0x00},  // 'r' (0x72)
    {0x48, 0x54, 0x54, 0x54, 0x20, 0x00},  // 's' (0x73)
    {0x04, 0x3F, 0x44, 0x40, 0x20, 0x00},  // 't' (0x74)
    {0x3C, 0x40, 0x40, 0x20, 0x7C, 0x00},  // 'u' (0x75)
    {0x1C, 0x20, 0x40, 0x20, 0x1C, 0x00},  // 'v' (0x76)
    {0x3C, 0x40, 0x30, 0x40, 0x3C, 0x00},  // 'w' (0x77)
    {0x44, 0x28, 0x10, 0x28, 0x44, 0x00},  // 'x' (0x78)
    {0x0C, 0x50, 0x50, 0x50, 0x3C, 0x00},  // 'y' (0x79)
    {0x44, 0x64, 0x54, 0x4C, 0x44, 0x00},  // 'z' (0x7A)
    {0x00, 0x08, 0x36, 0x41, 0x00, 0x00},  // '{' (0x7B)
    {0x00, 0x00, 0x7F, 0x00, 0x00, 0x00},  // '|' (0x7C)
    {0x00, 0x41, 0x36, 0x08, 0x00, 0x00},  // '}' (0x7D)
    {0x08, 0x04, 0x08, 0x10, 0x08, 0x00},  // '~' (0x7E)

    {0x22, 0x14, 0x7F, 0x2A, 0x14, 0x00},  // 0x7F: BTマーク
    {0x22, 0x14, 0x08, 0x14, 0x22, 0x00},  // 0x80: ✕印
    {0x7E, 0x41, 0x41, 0x7E, 0x00, 0x00},  // 0x81: バッテリー残量 0/5
    {0x7E, 0x71, 0x71, 0x7E, 0x00, 0x00},  // 0x82: バッテリー残量 2/5
    {0x7E, 0x7D, 0x7D, 0x7E, 0x00, 0x00},  // 0x83: バッテリー残量 4/5
    {0x7E, 0x7F, 0x7F, 0x7E, 0x00, 0x00},  // 0x84: バッテリー残量 5/5
    {0x08, 0x0C, 0x3E, 0x18, 0x08, 0x00},  // 0x85: 充電中
    {0x0C, 0x62, 0x7F, 0x62, 0x0C, 0x00},  // 0x86: USB（トライデント）
};



static uint8_t ssd1306_buffer[SSD1306_WIDTH * SSD1306_HEIGHT / 8];
static bool oled_present = false;
static bool need_redraw = true;
static volatile bool oled_usb_refresh_requested = false;

// UI 状態
static volatile uint8_t cur_slot = 0;
static volatile oled_bt_state_t cur_bt_state = OLED_BT_DISCONNECTED;
static volatile bool ui_ble_mode = true;
static volatile oled_bt_state_t ui_bt_state = OLED_BT_DISCONNECTED;
static volatile uint8_t ui_current_slot = 0;
static volatile bool ui_slot_bonded[4] = {false, false, false, false};
static volatile char ui_slot_os[4] = {'-', '-', '-', '-'};
static char ui_p1_dev[4] = "---";
static char ui_p2_dev[4] = "---";
static char ui_status_msg[32] = "Remapper [Slot 1]";
static char ui_peer_addr[24] = "";
static volatile int ui_tx_timer = 0;

static bool ssd1306_cmd(uint8_t cmd) {
    uint8_t buf[2] = {0x00, cmd};
    int res = i2c_write_timeout_us(I2C_PORT, SSD1306_I2C_ADDR, buf, 2, false, I2C_TIMEOUT_US);
    return res == 2;
}

static bool ssd1306_data(const uint8_t* data, size_t len) {
    uint8_t buf[129];
    while (len > 0) {
        size_t chunk = (len > 128) ? 128 : len;
        buf[0] = 0x40; // Co=0, D/C#=1 (Data)
        memcpy(buf + 1, data, chunk);
        int res = i2c_write_timeout_us(I2C_PORT, SSD1306_I2C_ADDR, buf, chunk + 1, false, I2C_TIMEOUT_US);
        if (res != (int)(chunk + 1)) {
            return false;
        }
        data += chunk;
        len -= chunk;
    }
    return true;
}

static void set_pixel(uint8_t x, uint8_t y, bool on) {
    if (x >= SSD1306_WIDTH || y >= SSD1306_HEIGHT) return;
    uint16_t idx = x + (y / 8) * SSD1306_WIDTH;
    if (on) {
        ssd1306_buffer[idx] |= (1 << (y % 8));
    } else {
        ssd1306_buffer[idx] &= ~(1 << (y % 8));
    }
}

// 6x8 フォント描画（128x32 に横21文字 x 縦4行配置）
static void draw_char_6x8(uint8_t x, uint8_t y, char ch) {
    unsigned char uch = (unsigned char)ch;
    if (uch < FONT_FIRST_CHAR || uch > FONT_LAST_CHAR) return;
    const uint8_t* cdata = font_data[uch - FONT_FIRST_CHAR];

    for (uint8_t col = 0; col < FONT_WIDTH; col++) {
        uint8_t p = cdata[col];
        for (uint8_t bit = 0; bit < 8; bit++) {
            set_pixel(x + col, y + bit, (p & (1 << bit)) != 0);
        }
    }
}

static void draw_string_6x8(uint8_t x, uint8_t y, const char* str) {
    while (*str && x + FONT_WIDTH <= SSD1306_WIDTH) {
        draw_char_6x8(x, y, *str);
        x += FONT_WIDTH;
        str++;
    }
}

void oled_init(void) {
    i2c_init(I2C_PORT, I2C_BAUDRATE);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    busy_wait_ms(50); // OLED パネルの起動待ち (Core1安全)

    // 初期通信確認 (Display Off コマンド)
    if (!ssd1306_cmd(0xAE)) {
        printf("[OLED] SSD1306 not detected on I2C0 (0x3C)\r\n");
        oled_present = false;
        return;
    }
    oled_present = true;

    ssd1306_cmd(0x20); ssd1306_cmd(0x00); // Horizontal addressing mode
    ssd1306_cmd(0xA8); ssd1306_cmd(SSD1306_HEIGHT - 1); // Multiplex ratio (0x1F for 32)
    ssd1306_cmd(0xD3); ssd1306_cmd(0x00); // Display offset
    ssd1306_cmd(0x40);                   // Start line 0
    ssd1306_cmd(0xA1);                   // Segment remap (column 127 mapped to SEG0)
    ssd1306_cmd(0xC8);                   // COM output scan direction (remapped)
#if SSD1306_HEIGHT == 32
    ssd1306_cmd(0xDA); ssd1306_cmd(0x02); // COM pins configuration for 32
    ssd1306_cmd(0x81); ssd1306_cmd(0x8F); // Contrast for 32
#else
    ssd1306_cmd(0xDA); ssd1306_cmd(0x12); // COM pins configuration for 64
    ssd1306_cmd(0x81); ssd1306_cmd(0xCF); // Contrast for 64
#endif
    ssd1306_cmd(0xA4);                   // Resume to RAM content
    ssd1306_cmd(0xA6);                   // Normal display
    ssd1306_cmd(0xD9); ssd1306_cmd(0xF1); // Pre-charge period
    ssd1306_cmd(0xDB); ssd1306_cmd(0x40); // VCOMH deselect level
    ssd1306_cmd(0x8D); ssd1306_cmd(0x14); // Enable charge pump
    ssd1306_cmd(0x2E);                   // Deactivate scroll
    ssd1306_cmd(0xAF);                   // Display ON

    printf("[OLED] SSD1306 %dx%d initialized on I2C0 (GP4/GP5)\r\n", SSD1306_WIDTH, SSD1306_HEIGHT);
    need_redraw = true;
}

void oled_set_mode(bool ble_mode) {
    if (ui_ble_mode != ble_mode) {
        ui_ble_mode = ble_mode;
        need_redraw = true;
    }
}

void oled_set_slot(uint8_t slot) {
    if (ui_current_slot != slot) {
        ui_current_slot = slot;
        need_redraw = true;
    }
}

void oled_set_bt_state(oled_bt_state_t state) {
    if (ui_bt_state != state) {
        ui_bt_state = state;
        need_redraw = true;
    }
}

void oled_set_slot_bonded(uint8_t slot, bool bonded) {
    if (slot < 4 && ui_slot_bonded[slot] != bonded) {
        ui_slot_bonded[slot] = bonded;
        need_redraw = true;
    }
}

void oled_set_slot_os(uint8_t slot, char os_char) {
    if (slot < 4 && ui_slot_os[slot] != os_char) {
        ui_slot_os[slot] = os_char;
        need_redraw = true;
    }
}

// W-01: USB Host P1/P2 のデバイス種別を設定 (port: 0=P1, 1=P2)
void oled_set_usb_port(uint8_t port, const char* dev_str) {
    if (port == 0) {
        if (strncmp(ui_p1_dev, dev_str, 3) != 0) {
            snprintf(ui_p1_dev, sizeof(ui_p1_dev), "%-3.3s", dev_str);
            need_redraw = true;
        }
    } else if (port == 1) {
        if (strncmp(ui_p2_dev, dev_str, 3) != 0) {
            snprintf(ui_p2_dev, sizeof(ui_p2_dev), "%-3.3s", dev_str);
            need_redraw = true;
        }
    }
}

void oled_set_status(const char* status_msg) {
    if (strncmp(ui_status_msg, status_msg, sizeof(ui_status_msg)) != 0) {
        strncpy(ui_status_msg, status_msg, sizeof(ui_status_msg) - 1);
        ui_status_msg[sizeof(ui_status_msg) - 1] = '\0';
        need_redraw = true;
    }
}

// W-02: 接続先ホストの MAC アドレスを設定
void oled_set_peer_addr(const char* peer_addr_str) {
    if (strncmp(ui_peer_addr, peer_addr_str, sizeof(ui_peer_addr)) != 0) {
        strncpy(ui_peer_addr, peer_addr_str, sizeof(ui_peer_addr) - 1);
        ui_peer_addr[sizeof(ui_peer_addr) - 1] = '\0';
        need_redraw = true;
    }
}

void oled_notify_tx(void) {
    ui_tx_timer = 3;
    need_redraw = true;
}

void oled_task(void) {
    if (!oled_present) {
        return;
    }

    static uint64_t last_refresh_us = 0;
    uint64_t now_us = time_us_64();

    // 約 25 FPS (40ms) または 状態変更時に描画
    if (!need_redraw && (now_us - last_refresh_us < 40000)) {
        return;
    }

    need_redraw = false;
    last_refresh_us = now_us;

    // Line 0: Mode icon (BT: 0x7F / USB: 0x86) + Status + TX electric icon (0x85) (21 chars, no brackets)
    bool is_bt_mode = (ui_bt_state == OLED_BT_CONNECTED) ? true : ui_ble_mode;
    const char* mode_icon = is_bt_mode ? "\x7F" : "\x86";
    const char* state_str = "DISC";
    switch (ui_bt_state) {
        case OLED_BT_CONNECTED: state_str = "CONN"; break;
        case OLED_BT_PAIRING:   state_str = "PAIR"; break;
        case OLED_BT_SWITCHING: state_str = "WAIT"; break;
        default:                state_str = "DISC"; break;
    }
    const char* tx_icon = " ";
    if (ui_tx_timer > 0) {
        tx_icon = "\x85"; // 0x85: 稲妻/電気アイコン (TX点滅)
        ui_tx_timer--;
    }
    char line0[24];
    snprintf(line0, sizeof(line0), "%s %-4s              %s", mode_icon, state_str, tx_icon);

    // Line 1: 各スロット状態 1:M* 2:W  3:A  4:I  (21 chars)
    char line1[24];
    char s_os[4];
    char s_star[4];
    for (int i = 0; i < 4; i++) {
        s_os[i] = ui_slot_os[i];
        if (!ui_slot_bonded[i] && (i != ui_current_slot)) {
            s_os[i] = '-';
        }
        s_star[i] = (i == ui_current_slot) ? '*' : ' ';
    }
    snprintf(line1, sizeof(line1), "1:%c%c 2:%c%c 3:%c%c 4:%c%c ",
             s_os[0], s_star[0],
             s_os[1], s_star[1],
             s_os[2], s_star[2],
             s_os[3], s_star[3]);

    // Line 2 (W-01): USB ポート情報 USB: P1:KBD  P2:MS   (21 chars)
    char line2[24];
    snprintf(line2, sizeof(line2), "USB: P1:%-3.3s  P2:%-3.3s  ", ui_p1_dev, ui_p2_dev);

    // Line 3 (W-02/W-14): ステータスメッセージ または BTアイコン左寄せ+MACアドレス右寄せ (21 chars)
    char line3[32];
    if (ui_bt_state == OLED_BT_CONNECTED && ui_peer_addr[0] != '\0') {
        int pad = 20 - (int)strlen(ui_peer_addr);
        if (pad < 0) pad = 0;
        snprintf(line3, sizeof(line3), "\x7F%*s%s", pad, "", ui_peer_addr);
    } else {
        snprintf(line3, sizeof(line3), "%-21.21s", ui_status_msg);
    }

    // バッファクリア
    memset(ssd1306_buffer, 0, sizeof(ssd1306_buffer));

    // 4行描画 (128x32: Y=0, 8, 16, 24)
    draw_string_6x8(0, 0, line0);
    draw_string_6x8(0, 8, line1);
    draw_string_6x8(0, 16, line2);
    draw_string_6x8(0, 24, line3);

    // 画面へ転送 (Horizontal addressing: Page 0..3)
    ssd1306_cmd(0x21); ssd1306_cmd(0); ssd1306_cmd(SSD1306_WIDTH - 1);
    ssd1306_cmd(0x22); ssd1306_cmd(0); ssd1306_cmd((SSD1306_HEIGHT / 8) - 1);
    ssd1306_data(ssd1306_buffer, sizeof(ssd1306_buffer));
}


// =========================================================================
// W-01/W-17: USB Host P1/P2 のデバイス種別（KBD/MS/DEV/---）を正確かつ安全に検出
// =========================================================================

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

void oled_update_usb_ports(void) {
    static uint32_t last_usb_check_ms = 0;
    uint32_t now_ms = (uint32_t)(time_us_64() / 1000);
    bool requested = oled_usb_refresh_requested;
    oled_usb_refresh_requested = false;
    if (requested || (now_ms - last_usb_check_ms >= 500)) {
        last_usb_check_ms = now_ms;
        update_usb_ports_status();
    }
}

void oled_request_usb_ports_refresh(void) {
    oled_usb_refresh_requested = true;
}
