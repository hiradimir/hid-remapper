// BL-39: Wi-Fi (UDP) 経由のリモートログ送信
//
// 構成:
//   - リングバッファ (IRQ マスク付き spin lock 保護) に全コンテキストから行単位で追記
//   - メインループの remote_log_task() が Wi-Fi 接続 (非同期) と UDP フラッシュを担当
//   - CYW43 (BLE と共有) のため、UDP 操作は cyw43_arch_lwip_begin/end で包む
//   - バッファ溢れ時は Drop-Oldest (最古破棄) — 不変条件 3 と同型の保全方針
//
// 注意:
//   - BLE (btstack) の cyw43_arch_init() は ble_hid_init() 側で 1 回だけ行うため、
//     ここでは再初期化しない
//   - lwIP は NO_SYS=1 (pico_cyw43_arch_lwip_threadsafe_background) のため、
//     lwIP API 呼び出しはメインループから cyw43_arch_lwip_begin/end を介して行う
#include "remote_log.h"

#if ENABLE_REMOTE_LOG

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <pico/cyw43_arch.h>
#include <pico/time.h>
extern "C" {
#include <hardware/sync/spin_lock.h>
}
#include <lwip/pbuf.h>
#include <lwip/udp.h>
#include <lwip/ip_addr.h>
#include <lwip/ip4_addr.h>
#include <lwip/inet.h>
#include <lwip/netif.h>

#if __has_include("remote_log_net.h")
#include "remote_log_net.h"
#else
#error "ENABLE_REMOTE_LOG requires src/remote_log_net.h (copy remote_log_net.h.example)"
#endif

#ifndef RLOG_WIFI_COUNTRY
#define RLOG_WIFI_COUNTRY 0
#endif

#ifndef RLOG_PING_INTERVAL_MS
#define RLOG_PING_INTERVAL_MS 5000
#endif

#ifndef RLOG_CONNECT_RETRY_MS
#define RLOG_CONNECT_RETRY_MS 30000
#endif

#ifndef RLOG_FLUSH_INTERVAL_MS
#define RLOG_FLUSH_INTERVAL_MS 20
#endif

#ifndef RLOG_DATAGRAM_MAX
#define RLOG_DATAGRAM_MAX 512
#endif

#define RLOG_RING_SIZE 4096
#define RLOG_LINE_MAX 200

// ---------------------------------------------------------------------------
// 状態
// ---------------------------------------------------------------------------

enum {
    WIFI_IDLE,       // 設定なし or 初期化待ち
    WIFI_CONNECTING, // 接続試行中
    WIFI_JOINED,     // IP 取得済み・UDP 送信可能
};

enum {
    UDP_CLOSED,
    UDP_OPENED,
};

static spin_lock_t *rl_lock = nullptr; // IRQ マスク付きロック (任意コンテキストの追記を保護)
static volatile bool rl_cs_ready = false;

static uint8_t rl_ring[RLOG_RING_SIZE];
static volatile size_t rl_head = 0; // 消費位置 (フラッシュ済み)
static volatile size_t rl_tail = 0; // 追記位置

static volatile uint32_t rl_drops = 0;      // Drop-Oldest による破棄行数
static volatile uint32_t rl_datagrams = 0;  // UDP 送信成功数
static volatile uint32_t rl_send_errors = 0;

static uint32_t count_ingest = 0; // main ループ: read_report で拾ったレポート数
static uint32_t count_forward = 0; // main ループ: 転送 (kyouhan) 実行数

static int wifi_state = WIFI_IDLE;
static int udp_state = UDP_CLOSED;
static struct udp_pcb *rl_pcb = nullptr;
static ip_addr_t rl_target_addr;
static bool rl_target_valid = false;

static uint64_t next_task_us = 0;       // タスク内部スロットリング
static uint64_t next_connect_attempt_us = 0;
static uint64_t next_ping_us = 0;

// ---------------------------------------------------------------------------
// リングバッファ (IRQ マスク付き spin lock 保護 — 任意コンテキストからの追記に対応)
// ---------------------------------------------------------------------------

static inline uint32_t rl_enter(void) {
    return spin_lock_blocking(rl_lock); // IRQ 禁止 + spin lock 取得
}

static inline void rl_exit(uint32_t save) {
    spin_unlock(rl_lock, save); // IRQ 状態を復元
}

static size_t ring_free_locked(void) {
    ssize_t used = (ssize_t) rl_tail - (ssize_t) rl_head;
    if (used < 0) used += RLOG_RING_SIZE;
    return (size_t) (RLOG_RING_SIZE - 1 - used);
}

// 最も古い行 ('\n' まで) を破棄する。行末が見つからない場合は全消去。
static void ring_drop_oldest_line_locked(void) {
    for (size_t i = rl_head; i != rl_tail; i = (i + 1) % RLOG_RING_SIZE) {
        if (rl_ring[i] == '\n') {
            rl_head = (i + 1) % RLOG_RING_SIZE;
            return;
        }
    }
    // 行末なし → 溜まっている内容はすべて破棄
    rl_head = rl_tail;
}

static void ring_write_locked(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        rl_ring[rl_tail] = data[i];
        rl_tail = (rl_tail + 1) % RLOG_RING_SIZE;
    }
}

// ---------------------------------------------------------------------------
// 公開 API (任意コンテキストから呼ばれるのは remote_log_eventf のみ)
// ---------------------------------------------------------------------------

void remote_log_eventf(const char *fmt, ...) {
    if (!rl_cs_ready) return;

    char line[RLOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    size_t len = (size_t) n;
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    line[len++] = '\n';
    if (len >= RLOG_RING_SIZE) return;

    uint32_t save = rl_enter();
    while (ring_free_locked() < len) {
        ring_drop_oldest_line_locked();
        rl_drops++;
    }
    ring_write_locked((const uint8_t *) line, len);
    rl_exit(save);
}

void remote_log_count_ingest(void) { count_ingest++; }
void remote_log_count_forward(void) { count_forward++; }

// ---------------------------------------------------------------------------
// UDP フラッシュ (メインループのみ)
// ---------------------------------------------------------------------------

// 未送信データを 1 データグラムにまとめて送る。失敗時は破棄せず次回再試行。
static void udp_flush_pending(uint64_t now_us) {
    if (udp_state != UDP_OPENED || rl_pcb == nullptr) return;
    (void) now_us;

    // 先頭から行 ('\n' 単位) を束ねる。最大 RLOG_DATAGRAM_MAX バイト。
    // 行末が確定しない (未完の行しか無い) 場合は何もせず次回へ。
    uint8_t chunk[RLOG_DATAGRAM_MAX];
    size_t chunk_len = 0;
    size_t new_head = rl_head;

    uint32_t save = rl_enter();
    for (size_t i = rl_head; i != rl_tail; i = (i + 1) % RLOG_RING_SIZE) {
        uint8_t c = rl_ring[i];
        chunk[chunk_len++] = c;
        bool line_complete = (c == '\n');
        bool chunk_full = (chunk_len >= RLOG_DATAGRAM_MAX);
        if (line_complete || chunk_full) {
            new_head = (i + 1) % RLOG_RING_SIZE;
            break;
        }
        if (((i + 1) % RLOG_RING_SIZE) == rl_tail) {
            // 未完の行で終端 − 行末が来るまで待つ
            chunk_len = 0;
            break;
        }
    }
    if (chunk_len == 0) {
        rl_exit(save);
        return;
    }
    rl_head = new_head;
    rl_exit(save);

    // UDP 送信 (lwIP 呼び出しは必ずメインループから cyw43_arch_lwip_begin で包む)
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t) chunk_len, PBUF_RAM);
    if (p == nullptr) {
        rl_send_errors++;
        return;
    }
    memcpy(p->payload, chunk, chunk_len);
    cyw43_arch_lwip_begin();
    err_t err = udp_send(rl_pcb, p);
    cyw43_arch_lwip_end();
    pbuf_free(p);

    if (err == ERR_OK) {
        rl_datagrams++;
    } else {
        // 送信失敗時は最古破棄で物流を止めない (たまに行頭も失われるが観測は継続)
        rl_send_errors++;
    }
}

// ---------------------------------------------------------------------------
// Wi-Fi 状態機械 (メインループのみ)
// ---------------------------------------------------------------------------

static const char *wifi_state_name(int s) {
    switch (s) {
        case WIFI_IDLE: return "IDLE";
        case WIFI_CONNECTING: return "CONN";
        case WIFI_JOINED: return "JOIN";
        default: return "?";
    }
}

static void udp_open_locked_helper(void) {
    cyw43_arch_lwip_begin();
    if (rl_pcb == nullptr) {
        rl_pcb = udp_new();
    }
    if (rl_pcb != nullptr && rl_target_valid) {
        udp_connect(rl_pcb, &rl_target_addr, RLOG_TARGET_PORT);
        udp_state = UDP_OPENED;
    }
    cyw43_arch_lwip_end();
    if (udp_state == UDP_OPENED) {
        remote_log_eventf("UDP ready -> %s:%d", RLOG_TARGET_IP, RLOG_TARGET_PORT);
    }
}

static void udp_close_cleanup(void) {
    if (rl_pcb != nullptr) {
        cyw43_arch_lwip_begin();
        udp_remove(rl_pcb);
        cyw43_arch_lwip_end();
        rl_pcb = nullptr;
    }
    udp_state = UDP_CLOSED;
}

static void wifi_task(uint64_t now_us) {
    if (RLOG_WIFI_SSID[0] == '\0') {
        return; // 設定なし → 完全惰性
    }

    int link = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);

    switch (wifi_state) {
        case WIFI_IDLE:
            if (now_us < next_connect_attempt_us) return;
            wifi_state = WIFI_CONNECTING;
            remote_log_eventf("WIFI connect start ssid=%s", RLOG_WIFI_SSID);
            cyw43_arch_wifi_connect_async(RLOG_WIFI_SSID, RLOG_WIFI_PASSWORD,
                                          RLOG_WIFI_PASSWORD[0] ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN);
            return;

        case WIFI_CONNECTING:
            if (link == CYW43_LINK_UP) {
                wifi_state = WIFI_JOINED;
                udp_open_locked_helper();
                const ip4_addr_t *ip = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
                remote_log_eventf("WIFI JOINED ip=%s", ip4addr_ntoa(ip));
            } else if (link < 0) {
                remote_log_eventf("WIFI FAIL st=%d (retry in %ds)", link, RLOG_CONNECT_RETRY_MS / 1000);
                wifi_state = WIFI_IDLE;
                next_connect_attempt_us = now_us + RLOG_CONNECT_RETRY_MS * 1000ULL;
                udp_close_cleanup();
            }
            return;

        case WIFI_JOINED:
            if (link != CYW43_LINK_UP) {
                remote_log_eventf("WIFI LOST st=%d", link);
                wifi_state = WIFI_IDLE;
                udp_close_cleanup();
                next_connect_attempt_us = now_us + 3000000ULL;
            }
            return;

        default:
            wifi_state = WIFI_IDLE;
            return;
    }
}

// ---------------------------------------------------------------------------
// メインループタスク
// ---------------------------------------------------------------------------

void remote_log_task(void) {
    uint64_t now_us = time_us_64();
    if (((int64_t) (next_task_us - now_us)) > 0) return;
    next_task_us = now_us + RLOG_FLUSH_INTERVAL_MS * 1000ULL;

    wifi_task(now_us);
    udp_flush_pending(now_us);

    // 5秒毎の生存ピング (リングが詰まっていても出す)
    if (((int64_t) (next_ping_us - now_us)) <= 0) {
        next_ping_us = now_us + RLOG_PING_INTERVAL_MS * 1000ULL;
        size_t pending = (size_t) ((ssize_t) rl_tail - (ssize_t) rl_head);
        if ((ssize_t) pending < 0) pending += RLOG_RING_SIZE;
        remote_log_eventf("ALIVE up=%ds wifi=%s in=%u fwd=%u q=%u dr=%u dg=%u se=%u",
                          (unsigned) (now_us / 1000000ULL), wifi_state_name(wifi_state),
                          (unsigned) count_ingest, (unsigned) count_forward,
                          (unsigned) pending, (unsigned) rl_drops,
                          (unsigned) rl_datagrams, (unsigned) rl_send_errors);
    }
}

void remote_log_init(void) {
    int lock_num = spin_lock_claim_unused(true);
    rl_lock = spin_lock_init((uint) lock_num);
    rl_cs_ready = true;
    rl_head = 0;
    rl_tail = 0;

    rl_target_valid = false;
    if (RLOG_TARGET_IP[0] != '\0') {
        rl_target_valid = ipaddr_aton(RLOG_TARGET_IP, &rl_target_addr);
    }

    const uint64_t boot_grace_us = 3000000ULL; // BLE 起動を優先させる 3 秒猶予
    next_connect_attempt_us = time_us_64() + boot_grace_us;
    next_ping_us = time_us_64() + 5000000ULL;

    remote_log_eventf("RLOG init target=%s:%d ssid=%s ok",
                      RLOG_TARGET_IP, RLOG_TARGET_PORT, RLOG_WIFI_SSID);
}

#endif // ENABLE_REMOTE_LOG
