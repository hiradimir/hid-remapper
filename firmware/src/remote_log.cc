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
#include <pico/stdio.h>
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
#include <lwip/dhcp.h>
#if __has_include("remote_log_net.h")
#include "remote_log_net.h"
#else
#error "ENABLE_REMOTE_LOG requires src/remote_log_net.h (copy remote_log_net.h.example)"
#endif

#include "oled.h"

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

// --- 静的 IP (任意; この 3 つが揃ったときだけ DHCP を停止して静的にする) ---
#ifndef RLOG_STATIC_IP
#define RLOG_STATIC_IP ""
#endif
#ifndef RLOG_STATIC_GATEWAY
#define RLOG_STATIC_GATEWAY ""
#endif
#ifndef RLOG_STATIC_NETMASK
#define RLOG_STATIC_NETMASK ""
#endif

// 1 でリングから排水した行を stdio (UART) にもミラーする (ベンチ bring-up 用)。
// メインループの排水時にのみ出すので、BLE リアルタイム経路に printf は発生しない
#ifndef RLOG_STDIO_MIRROR
#define RLOG_STDIO_MIRROR 1
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

static bool rl_static_valid = false;
static ip_addr_t rl_static_ip;
static ip_addr_t rl_static_gw;
static ip_addr_t rl_static_nm;

static uint64_t next_task_us = 0;       // タスク内部スロットリング
static uint64_t next_connect_attempt_us = 0;
static uint64_t connect_started_us = 0;
static uint64_t next_ping_us = 0;

#define RLOG_CONNECT_TIMEOUT_MS 45000 // 接続試行の滞留検知 (BADAUTH等のイベント無し滞留含む)

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
// UDP 未接続でも排水は行い (stdio ミラーで UART に出る)、bring-up を観測可能にする。
static void udp_flush_pending(uint64_t now_us) {
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

#if RLOG_STDIO_MIRROR
    // ベンチ観測用: 排水した行をそのまま UART へ
    printf("%.*s", (int) chunk_len, (const char *) chunk);
    stdio_flush();
#endif

    // UDP 送信 (lwIP 呼び出しは必ずメインループから cyw43_arch_lwip_begin で包む)。
    // pbuf_alloc/free も mem ヒープを触るためロック内に置く (RX 側は async helper の
    // IRQ 文脈で同じ mem プールを使うためロック外確保はヒープ競合になる)
    if (udp_state != UDP_OPENED || rl_pcb == nullptr) return;
    cyw43_arch_lwip_begin();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t) chunk_len, PBUF_RAM);
    if (p == nullptr) {
        cyw43_arch_lwip_end();
        rl_send_errors++;
        return;
    }
    memcpy(p->payload, chunk, chunk_len);
    err_t err = udp_send(rl_pcb, p);
    pbuf_free(p);
    cyw43_arch_lwip_end();

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

// 静的 IP の適用 (DHCP を止めて直接アドレスを置く)。再接続後の再適用は netif の
// アドレスが未設定 (0.0.0.0) のときに行うので冪等
static void apply_static_addr(void) {
    struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];
    cyw43_arch_lwip_begin();
    bool need = (ip_2_ip4(&n->ip_addr)->addr == 0);
    if (need) {
        struct dhcp *d = netif_dhcp_data(n);
        if (d != nullptr) {
            dhcp_stop(n);
        }
        netif_set_addr(n, &rl_static_ip, &rl_static_nm, &rl_static_gw);
    }
    cyw43_arch_lwip_end();
    if (need) {
        remote_log_eventf("STATIC IP %s gw=%s nm=%s",
                          ip4addr_ntoa(ip_2_ip4(&rl_static_ip)),
                          ip4addr_ntoa(ip_2_ip4(&rl_static_gw)),
                          ip4addr_ntoa(ip_2_ip4(&rl_static_nm)));
    }
}

static void wifi_task(uint64_t now_us) {
    if (RLOG_WIFI_SSID[0] == '\0') {
        return; // 設定なし → 完全惰性
    }

    // 注意: ここは IP 取得まで見る cyw43_tcpip_link_status() を使うこと。
    // cyw43_wifi_link_status() はドライバレベル (関連付けのみ) で、関連付け後は
    // 永遠に CYW43_LINK_JOIN のまま返すため DHCP 完了 (LINK_UP) を検出できない
    int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);

    switch (wifi_state) {
        case WIFI_IDLE:
            if (now_us < next_connect_attempt_us) return;
            wifi_state = WIFI_CONNECTING;
            connect_started_us = now_us;
            remote_log_eventf("WIFI connect start ssid=%s", RLOG_WIFI_SSID);
            oled_set_wifi_state(".");
            cyw43_arch_wifi_connect_async(RLOG_WIFI_SSID, RLOG_WIFI_PASSWORD,
                                          RLOG_WIFI_PASSWORD[0] ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN);
            return;

        case WIFI_CONNECTING:
            if (link == CYW43_LINK_UP) {
                wifi_state = WIFI_JOINED;
                udp_open_locked_helper();
                const ip4_addr_t *ip = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
                remote_log_eventf("WIFI JOINED ip=%s", ip4addr_ntoa(ip));
                oled_set_wifi_state("*");
            } else if (link < 0) {
                char code[4];
                snprintf(code, sizeof(code), "%d", link);
                remote_log_eventf("WIFI FAIL st=%s (retry in %ds)", code, RLOG_CONNECT_RETRY_MS / 1000);
                oled_set_wifi_state(code);
                wifi_state = WIFI_IDLE;
                next_connect_attempt_us = now_us + RLOG_CONNECT_RETRY_MS * 1000ULL;
                udp_close_cleanup();
            } else if (link == CYW43_LINK_NOIP && rl_static_valid) {
                // DHCP を待たず静的アドレスを置く (再接続後も 0.0.0.0 になったら再適用)
                apply_static_addr();
            } else if (connect_started_us != 0 &&
                       now_us > connect_started_us + RLOG_CONNECT_TIMEOUT_MS * 1000ULL) {
                // イベント無しの滞留 (スキャン帰属なし等) を検知して再試行。
                // 診断値の読み取りは lwIP 構造体なので async_context ロック内で行う
                int dhcp_to = -1;
                unsigned js_to = 0;
                unsigned nf_to = 0;
                cyw43_arch_lwip_begin();
                {
                    struct netif *nif_to = &cyw43_state.netif[CYW43_ITF_STA];
                    struct dhcp *d_to = (nif_to != nullptr) ? netif_dhcp_data(nif_to) : nullptr;
                    if (d_to != nullptr) dhcp_to = (int) d_to->state;
                    js_to = (unsigned) cyw43_state.wifi_join_state;
                    nf_to = (unsigned) nif_to->flags;
                }
                cyw43_arch_lwip_end();
                char code[4];
                remote_log_eventf("WIFI TO st=%d dhcp=%d js=0x%x nf=0x%x (retry in %ds)", link, dhcp_to,
                                  js_to, nf_to, RLOG_CONNECT_RETRY_MS / 1000);
                // OLED は T<dhcp 状態> 表示 (2=INIT は DISCOVER 未発火, 6=SELECTING は
                // DISCOVER 済み OFFER 待ち, 1=REQUESTING は ACK 待ちなどを判別するためのもの)
                snprintf(code, sizeof(code), "T%d", dhcp_to);
                oled_set_wifi_state(code);
                // 進行中の join を明示的に畳む (disassociate) してから 30s 後の再試行に
                // 備える。再試行の connect_async は前試行を abort しないため、
                // 滞留した join 状態が次試行のイベント流に混線するのを防ぐ。
                // (メインループ経由の呼び出しに限定・BLE 体制には触れない)
                cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
                udp_close_cleanup();
                wifi_state = WIFI_IDLE;
                next_connect_attempt_us = now_us + RLOG_CONNECT_RETRY_MS * 1000ULL;
            }
            return;

        case WIFI_JOINED:
            if (link != CYW43_LINK_UP) {
                remote_log_eventf("WIFI LOST st=%d", link);
                oled_set_wifi_state("?");
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

        //診断: DHCP 状態機の位置も添える (lwIP: 2=INIT 6=SELECTING 1=REQUESTING 10=BOUND)。
        // js=cyw43 wifi_join_state 生値 (0x0e01 で全フラグ到達=join 完了) と
        // nf=netif->flags 16進 (0x9=UP|LINK_UP|ETHARP 相当) も添える
        struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];
        int dhcp_state = -1;
        int dhcp_tries = 0;
        unsigned js = 0;
        unsigned nf = 0;
        const char *ip_str = "0.0.0.0";
        static char ip_buf[16];
        cyw43_arch_lwip_begin();
        if (nif != nullptr) {
            struct dhcp *d = netif_dhcp_data(nif);
            if (d != nullptr) {
                dhcp_state = (int) d->state;
                dhcp_tries = (int) d->tries;
            }
            js = (unsigned) cyw43_state.wifi_join_state;
            nf = (unsigned) nif->flags;
            strncpy(ip_buf, ip4addr_ntoa(netif_ip4_addr(nif)), sizeof(ip_buf) - 1);
            ip_buf[sizeof(ip_buf) - 1] = '\0';
            ip_str = ip_buf;
        }
        cyw43_arch_lwip_end();

        remote_log_eventf("ALIVE up=%ds wifi=%s(%d) ip=%s dhcp=%d/%d js=0x%x nf=0x%x in=%u fwd=%u q=%u dr=%u dg=%u se=%u",
                          (unsigned) (now_us / 1000000ULL), wifi_state_name(wifi_state),
                          cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA),
                          ip_str, dhcp_state, dhcp_tries, js, nf,
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

    // STA モードの起動 (cyw43 自体は ble_hid_init() 内で cyw43_arch_init() 済み)。
    // ※これを呼ばないと STA netif が立たず link は永遠に DOWN のまま何も起きない。
    // カントリー指定 (0 以外) はドライバ API に直接渡す。既定値 0 は無効値
    // (CyW43_COUNTRY_WORLDWIDE 'W''W' ではない) のため enable_sta_mode 経由
    // (WORLDWIDE 既定) に落とす。
    // 注意: country 値は #if で評価できない (CYW43_COUNTRY() マクロに cast を含む
    // ためプリプロセッサ演算エラーになる) → 実行時判定に移した
    {
        uint32_t rlog_country = RLOG_WIFI_COUNTRY;
        if (rlog_country != 0) {
            cyw43_wifi_set_up(&cyw43_state, CYW43_ITF_STA, true, rlog_country);
        } else {
            cyw43_arch_enable_sta_mode();
        }
    }

    rl_target_valid = false;
    if (RLOG_TARGET_IP[0] != '\0') {
        rl_target_valid = ipaddr_aton(RLOG_TARGET_IP, &rl_target_addr);
    }

    // 静的 IP (3 揃いで有効)
    rl_static_valid =
        RLOG_STATIC_IP[0] != '\0' && RLOG_STATIC_GATEWAY[0] != '\0' && RLOG_STATIC_NETMASK[0] != '\0' &&
        ipaddr_aton(RLOG_STATIC_IP, &rl_static_ip) &&
        ipaddr_aton(RLOG_STATIC_GATEWAY, &rl_static_gw) &&
        ipaddr_aton(RLOG_STATIC_NETMASK, &rl_static_nm);
    if (rl_static_valid) {
        apply_static_addr();
    }
    remote_log_eventf("RLOG init target=%s:%d ssid=%s ip=%s",
                      RLOG_TARGET_IP, RLOG_TARGET_PORT, RLOG_WIFI_SSID,
                      rl_static_valid ? "static" : "dhcp");

    const uint64_t boot_grace_us = 3000000ULL; // BLE 起動を優先させる 3 秒猶予
    next_connect_attempt_us = time_us_64() + boot_grace_us;
    next_ping_us = time_us_64() + 5000000ULL;

    remote_log_eventf("RLOG init target=%s:%d ssid=%s ok",
                      RLOG_TARGET_IP, RLOG_TARGET_PORT, RLOG_WIFI_SSID);
}

#endif // ENABLE_REMOTE_LOG
