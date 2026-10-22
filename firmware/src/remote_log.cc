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
#include <lwip/etharp.h>
#if __has_include("remote_log_net.h")
#include "remote_log_net.h"
#else
#error "ENABLE_REMOTE_LOG requires src/remote_log_net.h (copy remote_log_net.h.example)"
#endif

#include "build_id.h"
#include "oled.h"
#include "ble_hid.h"

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

// BLE ヘルス観測 (BL-43/45 ウェッジ用)
#ifndef RLOG_BLE_HB_MS
#define RLOG_BLE_HB_MS 15000 // BLE ヘルス行の周期 (変化が無ければ接続中のみ)
#endif
#ifndef RLOG_BLE_STALL_MS
#define RLOG_BLE_STALL_MS 5000 // 通知がこれだけ途絶えたら「停止」とみなす
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
#define RLOG_STDIO_MIRROR 0
#endif

// リングサイズ (バイト)。remote_log_net.h から上書き可。RAM を静的配列として消費する
#ifndef RLOG_RING_SIZE
#define RLOG_RING_SIZE 16384
#endif
#define RLOG_LINE_MAX 256

// データグラム先頭に付ける "#<seq> " プレフィックスの最大長 (余裕含む 16 バイト)。
// 抽出側は RLOG_DATAGRAM_MAX からこの分を引いた長さを上限とする
#define RLOG_SEQ_PREFIX_MAX 16

// ACK ベース簡易再送 (stop-and-wait)。RLOG_ACK_ENABLE=0 で機構ごと無効 (投げっぱなし)。
// Receiver から ACK が 1 度も返らない環境 (nc -lu 等) では RLOG_ACK_GIVEUP_MS 経過で
// 自動的に投げっぱなしにフォールバックする (--no-ack 運用で止まらない)
#ifndef RLOG_ACK_ENABLE
#define RLOG_ACK_ENABLE 1
#endif
#ifndef RLOG_ACK_TIMEOUT_MS
// ACK 待ちの再送間隔。実測で AP (Mac のインターネット共有) の RTT が 400ms 超に
// なることがあり、500ms では正常な ACK を取りこぼして再送 (rtx) が積み上がった。
// 1.2 秒に広げ、実測 RTT は BLE-HB の rtt=/rmax= で監視する
#define RLOG_ACK_TIMEOUT_MS 1200
#endif
#ifndef RLOG_ACK_GIVEUP_MS
#define RLOG_ACK_GIVEUP_MS 60000 // 開通後これだけ ACK が 1 度も返らなければ fire&forget へ。
// 実機で「Pico 起動 → 数十秒後に listener 起動」の運用があり、短いと giveup 時点で
// 起動〜開通の履歴 (seq 1..N) が送り直されないまま消える。60 秒にすればリング保持分
// (16KB ≒ 100 行) が再送で復元される。後から ACK が届けば ACK モードへ自動復帰する
#endif

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
// 絶対カーソル (リング位置は abs % RLOG_RING_SIZE)。u64 なので数値比較で
// 前後関係・巻き戻し判定ができる
static volatile uint64_t rl_tail_abs = 0;   // 追記位置 (完結行のみ一括で書かれる)
static volatile uint64_t rl_head_abs = 0;   // 送信カーソル (udp_send 成功時のみ前進)
static volatile uint64_t rl_mirror_abs = 0; // UART ミラー用カーソル (RLOG_STDIO_MIRROR 時前進)

static volatile uint32_t rl_drops = 0;      // Drop-Oldest による破棄行数
static volatile uint32_t rl_datagrams = 0;  // UDP 送信成功数
static volatile uint32_t rl_send_errors = 0;
static volatile uint32_t rl_seq = 0;      // 送信済みデータグラム通番 (送信成功ごとに +1)
static volatile uint32_t reconnect_count = 0; // WIFI LOST 検出回数 (BADAUTH 頻度の指標)
static int rl_last_err = 0; // 直近の udp_send エラーコード (err_t: -1=ERR_MEM -3=ERR_TIMEOUT -4=ERR_RTE)
static int rl_target_arp_state = -1; // 宛先 ARP 状態: -1=未調査 1=解決 2=未解決 (変化時のみ報告)
static volatile uint32_t rl_last_ack = 0; // 受信 "ACK <seq>" の最終値 (未達 = seq - ack で読む)
static volatile uint32_t ack_rtt_last_ms = 0; // 直近 ACK の往復時間
static volatile uint32_t ack_rtt_max_ms = 0;  // HB 区間内の最大往復時間
static volatile uint32_t inflight_seq = 0; // 送信中 (未 ACK) の seq、0 = なし (stop-and-wait)
static volatile uint64_t inflight_end_abs = 0; // inflight 行の終端リング位置 (送信成功時の cend)
static uint64_t inflight_sent_us = 0;          // inflight の送信 (または最終再送) 時刻
static uint64_t first_send_us = 0;             // ACK 検証の起点 (最初の成功送信時刻)
static volatile uint32_t rtx_count = 0;        // 再送回数 (ALIVE の rtx=)
static volatile bool ack_mode_off = false;     // ACK が返らない環境の fire&forget フォールバック

static uint32_t count_ingest = 0;  // main ループ: read_report で拾ったレポート数
static uint32_t count_forward = 0; // main ループ: 転送 (kyouhan) 実行数
static uint32_t loop_count = 0;        // main ループの周回数 (ループ速度の観測用)
static uint64_t last_loop_us = 0;      // 直近のループ時刻
static uint32_t loop_gap_max_us = 0;   // ループ間隔の最大値 (ファーム側スタックの検出)
static uint64_t last_ingest_us = 0;          // 直近の入力時刻
static uint32_t ingest_gap_max_us = 0;       // 入力レポート間隔の最大値 (途切れ検出)

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

// ログに出す SSID のマスク。ログは実機検証の証跡として外部に残るため、先頭 6 文字だけ
// 残して以降を伏せる (伏字は固定 5 個なので元の長さも漏らさない)。
// 戻り値は静的バッファ。1 つの printf に複数回渡さないこと
static const char* rlog_ssid_masked(void) {
    static char buf[16];
    size_t n = 0;
    while (n < 6 && RLOG_WIFI_SSID[n] != '\0') {
        buf[n] = RLOG_WIFI_SSID[n];
        n++;
    }
    if (RLOG_WIFI_SSID[n] == '\0') {
        buf[n] = '\0'; // 6 文字以下はそのまま (伏せる余地がない)
    } else {
        memcpy(buf + n, "*****", 5);
        buf[n + 5] = '\0';
    }
    return buf;
}

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
    uint64_t used = rl_tail_abs - rl_head_abs;
    return (size_t) (RLOG_RING_SIZE - 1 - used);
}

// 最も古い行 ('\n' まで) を破棄する。行末が見つからない場合は全消去。
// 捨てた領域を指していたミラーカーソルは送信位置まで揃える (捨て領域の再表示防止)
static void ring_drop_oldest_line_locked(void) {
    bool found_nl = false;
    for (uint64_t i = rl_head_abs; i != rl_tail_abs; i++) {
        if (rl_ring[i % RLOG_RING_SIZE] == '\n') {
            rl_head_abs = i + 1;
            found_nl = true;
            break;
        }
    }
    if (!found_nl) {
        // 行末なし → 溜まっている内容はすべて破棄
        rl_head_abs = rl_tail_abs;
    }
    if (rl_mirror_abs < rl_head_abs) {
        rl_mirror_abs = rl_head_abs;
    }
}

static void ring_write_locked(const uint8_t *data, size_t len) {
    for (size_t j = 0; j < len; j++) {
        rl_ring[rl_tail_abs % RLOG_RING_SIZE] = data[j];
        rl_tail_abs++;
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

void remote_log_count_ingest(void) {
    // 入力レポートの「間隔の最大値」を取る。5 秒平均の in= では見えない
    // 数百 ms の飛び (マウスのかくつき) を拾うため。main ループからのみ呼ばれる
    uint64_t now = time_us_64();
    if (last_ingest_us != 0) {
        uint32_t gap = (uint32_t) (now - last_ingest_us);
        if (gap > ingest_gap_max_us) {
            ingest_gap_max_us = gap;
        }
    }
    last_ingest_us = now;
    count_ingest++;
}
void remote_log_count_forward(void) { count_forward++; }

void remote_log_count_loop(void) {
    // ループ間隔の最大値を取る。ここが跳ねれば「ファームが止まった」ので、
    // 入力の空白 (gmax) が USB 側かファーム側かを切り分けられる
    uint64_t now = time_us_64();
    if (last_loop_us != 0) {
        uint32_t gap = (uint32_t) (now - last_loop_us);
        if (gap > loop_gap_max_us) {
            loop_gap_max_us = gap;
        }
    }
    last_loop_us = now;
    loop_count++;
}

// ---------------------------------------------------------------------------
// UDP フラッシュ (メインループのみ)
// ---------------------------------------------------------------------------

// 行境界 ('\n') 単位でリングから 1 区間取り出す。成功時 *out_end_abs に新カーソル。
// 追記側は常に完結行のみ書くため、通常は最初の '\n' で確実に terminate する
// (max_bytes 到達や未完行はデータ異常時の保険として扱う)。何も取り出せないとき 0
static size_t ring_extract_line_locked(uint64_t from_abs, uint8_t *chunk, size_t max_bytes,
                                       uint64_t *out_end_abs) {
    *out_end_abs = from_abs;
    if (from_abs >= rl_tail_abs) return 0;
    size_t len = 0;
    while (len < max_bytes) {
        uint64_t p = from_abs + len;
        if (p >= rl_tail_abs) {
            // 未完の行で終端 → 行末を待つ (前進させない)
            return 0;
        }
        uint8_t c = rl_ring[p % RLOG_RING_SIZE];
        chunk[len++] = c;
        if (c == '\n') {
            *out_end_abs = from_abs + len;
            return len;
        }
    }
    // max_bytes に達しても '\n' なし (異常データ保険) → 詰まり防止のため切って前進
    *out_end_abs = from_abs + len;
    return len;
}

static void udp_flush_pending(void);

// イベント行の先頭に載せる uptime (秒)。起動から開通までの時間が追える
static unsigned rlog_upt_s(void) {
    return (unsigned) (time_us_64() / 1000000ULL);
}

// 行 (プレフィックス無しの純行) を "#<seq> " を付けて 1 データグラムで送る。
// lwIP 呼び出し (alloc/free を含む) は async_context ロック内で必須
// (RX 側は async helper の IRQ 文脈で同じ mem プールを使うため)。
// 呼び出し元はメインループのみ
static err_t rl_udp_send_seq(uint32_t seq, const uint8_t *line, size_t len) {
    char prefix[RLOG_SEQ_PREFIX_MAX];
    int plen = snprintf(prefix, sizeof(prefix), "#%u ", (unsigned) seq);
    size_t pl = (plen > 0 && (size_t) plen < sizeof(prefix)) ? (size_t) plen : 0;
    size_t total = pl + len;
    if (total > RLOG_DATAGRAM_MAX) { // 異常データ保険: 総長はデータグラム上限内に切る
        len = RLOG_DATAGRAM_MAX - pl;
        total = RLOG_DATAGRAM_MAX;
    }
    err_t ret = ERR_MEM;
    cyw43_arch_lwip_begin();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t) total, PBUF_RAM);
    if (p != nullptr) {
        memcpy(p->payload, prefix, pl);
        memcpy((uint8_t *) p->payload + pl, line, len);
        ret = udp_send(rl_pcb, p);
        pbuf_free(p);
    }
    cyw43_arch_lwip_end();
    return ret;
}

// 送信エラーの記録。se (rl_send_errors) が 0 から非 0 に変わった瞬間、または
// エラーコードが前回と変わったときにだけ 1 行イベントを出す (イベント乱発を防ぐ)。
// メインループ経路からのみ呼ぶ (イベント行はリング経由なのでリアルタイム経路非経由)
static void rl_send_err_done(err_t err) {
    bool first = (rl_send_errors == 0);
    bool changed = ((int) err != rl_last_err);
    rl_last_err = (int) err;
    rl_send_errors++;
    if (first) {
        remote_log_eventf("RLOG send error first err=%d se=%u",
                          (int) err, (unsigned) rl_send_errors);
    } else if (changed) {
        remote_log_eventf("RLOG send error err=%d se=%u",
                          (int) err, (unsigned) rl_send_errors);
    }
}

// UDP フラッシュ + UART ミラー排水 (メインループ・20ms 間隔)。
//
// カーソルの整合:
//   - 送信カーソル (rl_head_abs) は UDP 未開通・送信失敗・pbuf 確保失敗のとき
//     一切前進しない → 開通後にバックログがまとめて送られる
//   - stop-and-wait (RLOG_ACK_ENABLE): 送信成功後もカーソルは前進させず
//     inflight_seq を設定して ACK を待つ。ACK 済み (rl_last_ack >= inflight_seq)
//     で初めて前進する。タイムアウトで同じ行を同じ seq で再送するため、
//     ネットワークで落ちた datagram はリングに残り続ける
//   - ACK が返らない環境は RLOG_ACK_GIVEUP_MS 経過で ack_mode_off = true に落ち、
//     以降は従来通り送信成功で即前進する (--no-ack 運用で停止しない)
//   - ミラーカーソル (rl_mirror_abs) は UDP 状態と無関係に前進する (起動直後の
//     行が UART に残る)。Drop-Oldest で捨てられた領域は
//     ring_drop_oldest_line_locked() がミラーカーソルを揃えることで二重表示を防ぐ
//   - 送信カーソルとミラーカーソルでそれぞれ 1 ティック 1 チャンクまで
//     (バックログ大量時も 20ms 間隔のレート制限が維持される)
static void udp_flush_pending(void) {
#if RLOG_STDIO_MIRROR
    {
        uint8_t mchunk[RLOG_DATAGRAM_MAX];
        uint64_t mend = 0;
        size_t mlen = 0;
        uint32_t save = rl_enter();
        // リングを 1 周以上取り残していたら送信位置まで飛ばす (捨て領域の再表示防止)
        if (rl_mirror_abs + RLOG_RING_SIZE <= rl_head_abs) {
            rl_mirror_abs = rl_head_abs;
        }
        mlen = ring_extract_line_locked(rl_mirror_abs, mchunk, sizeof(mchunk), &mend);
        if (mlen > 0) {
            rl_mirror_abs = mend;
        }
        rl_exit(save);
        if (mlen > 0) {
            printf("%.*s", (int) mlen, (const char *) mchunk);
            stdio_flush();
        }
    }
#endif

#if RLOG_ACK_ENABLE
    // (0) ACK 自動復帰: fire&forget に落ちていても後から ACK が届くことがある
    // (Pico 起動後に listener を起動する運用) → ACK モードへ戻す
    if (ack_mode_off && rl_last_ack != 0) {
        ack_mode_off = false;
        remote_log_eventf("RLOG ACK enable t=%us (ack=%u)", rlog_upt_s(),
                          (unsigned) rl_last_ack);
    }
    if (!ack_mode_off) {
        // (1) ACK 到着: inflight が ACK 済みなら、ここで初めて送信カーソルを前進させる
        if (inflight_seq != 0 && rl_last_ack >= inflight_seq) {
            uint32_t save = rl_enter();
            if (rl_head_abs < inflight_end_abs) {
                rl_head_abs = inflight_end_abs;
            }
            rl_exit(save);
            inflight_seq = 0;
        }
        // (2) フォールバック: ACK が 1 度も返らないまま GIVEUP 経過 → fire&forget へ
        if (first_send_us != 0 && rl_last_ack == 0 &&
            (time_us_64() - first_send_us) >= RLOG_ACK_GIVEUP_MS * 1000ULL) {
            ack_mode_off = true;
            if (inflight_seq != 0) {
                // 待っていた行は最低 1 回は送信済み → 投げっぱなしに切り替えて解放
                uint32_t save = rl_enter();
                if (rl_head_abs < inflight_end_abs) {
                    rl_head_abs = inflight_end_abs;
                }
                rl_exit(save);
                inflight_seq = 0;
            }
            remote_log_eventf("RLOG ACK disable t=%us (no ack in %us) -> fire-forget",
                              rlog_upt_s(), (unsigned) (RLOG_ACK_GIVEUP_MS / 1000));
        }
    }
#endif

    // (3) inflight 未解決: ACK 待ち or 再送
    if (inflight_seq != 0) {
#if RLOG_ACK_ENABLE
        uint64_t elapsed = time_us_64() - inflight_sent_us;
        if (!ack_mode_off && elapsed < RLOG_ACK_TIMEOUT_MS * 1000ULL) {
            return; // ACK 待ち (次ティックも待つ)
        }
#endif
        if (inflight_end_abs <= rl_head_abs) {
            // 待機中に Drop-Oldest で行が破棄された → 諦めて次の行へ
            inflight_seq = 0;
            return;
        }
        // 同じ行を同じ seq で再送 (カーソルは動かさない)
        uint8_t rchunk[RLOG_DATAGRAM_MAX - RLOG_SEQ_PREFIX_MAX];
        uint64_t rcend = 0;
        size_t rlen = 0;
        uint32_t save = rl_enter();
        rlen = ring_extract_line_locked(rl_head_abs, rchunk, sizeof(rchunk), &rcend);
        rl_exit(save);
        if (rlen == 0) { // 行が消えた (異常) → 解放して次へ
            inflight_seq = 0;
            return;
        }
        err_t err = rl_udp_send_seq(inflight_seq, rchunk, rlen);
        if (err == ERR_OK) {
            rl_datagrams++;
            inflight_sent_us = time_us_64();
            rtx_count++;
            return;
        }
        rl_send_err_done(err); // 再送失敗: 次ティックで再試行
        return;
    }

    // (4) 新規行の送信: 直前に inflight が未解決でないときのみ到達する
    if (udp_state != UDP_OPENED || rl_pcb == nullptr) {
        return; // 未開通: 送信カーソルはそのまま (この区間は後でまとめて送る)
    }

    // 送信カーソルから行境界までを取り出す (前進はまだしない)。
    // プレフィックスは rl_udp_send_seq 側で組み立てるため、先頭余白は不要
    uint8_t chunk[RLOG_DATAGRAM_MAX - RLOG_SEQ_PREFIX_MAX];
    uint64_t cend = 0;
    size_t clen = 0;
    {
        uint32_t save = rl_enter();
        clen = ring_extract_line_locked(rl_head_abs, chunk, sizeof(chunk), &cend);
        rl_exit(save);
    }
    if (clen == 0) return;

    uint32_t seq = rl_seq + 1u; // 送信に成功したときだけ rl_seq に採用
    err_t err = rl_udp_send_seq(seq, chunk, clen);
    if (err == ERR_OK) {
        rl_datagrams++;
        rl_seq = seq;
        if (RLOG_ACK_ENABLE && !ack_mode_off) {
            // stop-and-wait: ここではカーソルを前進させない。
            // ACK 済みになったら先頭の (1) で前進する
            inflight_seq = seq;
            inflight_end_abs = cend;
            inflight_sent_us = time_us_64();
            if (first_send_us == 0) {
                first_send_us = time_us_64(); // ACK 検証 (giveup) の起点
            }
        } else {
            // fire&forget: 送信成功で即前進 (Drop-Oldest が先へ進めていたら巻き戻さない)
            uint32_t save = rl_enter();
            if (rl_head_abs < cend) {
                rl_head_abs = cend;
            }
            rl_exit(save);
        }
    } else {
        rl_send_err_done(err); // 送信失敗: カーソル前進なし (次ティックで再試行)
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

// udp_recv コールバック。async helper の IRQ 文脈で走るため printf / ロック取得 /
// メモリ確保は禁止し、軽い解析と volatile 代入のみ行う。pbuf は必ず解放する
static void rl_udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                           const ip_addr_t *addr, u16_t port) {
    (void) arg; (void) pcb; (void) addr; (void) port;
    if (p == nullptr) return;
    // スキャッタ/ギャザ (pbuf チェーン) に備えて pbuf_copy_partial で最大 15 バイト
    // だけ取る (メモリ確保なし・チェーン追跡は lwIP 内部で完結)
    char buf[16];
    u16_t n = pbuf_copy_partial(p, buf, sizeof(buf) - 1, 0);
    if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    buf[n] = '\0';
    // "ACK <数字列>" のみ受け付ける (strtoul 相当の 10 進手書き解析)
    if (n >= 5 && buf[0] == 'A' && buf[1] == 'C' && buf[2] == 'K' && buf[3] == ' ') {
        uint32_t v = 0;
        for (u16_t i = 4; i < n && buf[i] >= '0' && buf[i] <= '9'; i++) {
            v = v * 10u + (uint32_t) (buf[i] - '0');
        }
        // 単調更新のみ (古い番号の遅延 ACK で巻き戻さない)
        if (v > rl_last_ack) {
            rl_last_ack = v;
            // 送信 (または最終再送) からの往復時間を測る。inflight が無ければ測れない
            uint32_t ifs = inflight_seq;
            if (ifs != 0 && v >= ifs) {
                uint32_t rtt = (uint32_t) ((time_us_64() - inflight_sent_us) / 1000ULL);
                ack_rtt_last_ms = rtt;
                if (rtt > ack_rtt_max_ms) {
                    ack_rtt_max_ms = rtt;
                }
            }
        }
    }
    pbuf_free(p);
}

static void udp_open_locked_helper(void) {
    cyw43_arch_lwip_begin();
    if (rl_pcb == nullptr) {
        rl_pcb = udp_new();
    }
    if (rl_pcb != nullptr && rl_target_valid) {
        udp_connect(rl_pcb, &rl_target_addr, RLOG_TARGET_PORT);
        // ACK 受信用コールバック (IRQ 文脈で呼ばれる)
        udp_recv(rl_pcb, rl_udp_recv_cb, nullptr);
        udp_state = UDP_OPENED;
    }
    cyw43_arch_lwip_end();
    if (udp_state == UDP_OPENED) {
        remote_log_eventf("UDP ready t=%us -> %s:%d", rlog_upt_s(), RLOG_TARGET_IP, RLOG_TARGET_PORT);
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
    // 未 ACK の行は送信カーソル (rl_head_abs) のままリングに残す →
    // 再接続後に新しい seq で送り直される。inflight と giveup 起点は畳む。
    // ARP 状態もクリア (再接続後の解決判定を新規に出させる)
    inflight_seq = 0;
    first_send_us = 0;
    rl_target_arp_state = -1;
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
        // ip4addr_ntoa は静的バッファを回して返すため 1 つの printf に複数回渡すと
        // 全部が最後の値 (netmask) になる (実機発覚: 「255.255.255.0 gw=255.255.255.0
        //  nm=255.255.255.0」)。ip4addr_ntoa_r で個別バッファに書いてから渡す
        char s_ip[16], s_gw[16], s_nm[16];
        remote_log_eventf("STATIC IP t=%us %s gw=%s nm=%s",
                          rlog_upt_s(),
                          ip4addr_ntoa_r(ip_2_ip4(&rl_static_ip), s_ip, (int) sizeof(s_ip)),
                          ip4addr_ntoa_r(ip_2_ip4(&rl_static_gw), s_gw, (int) sizeof(s_gw)),
                          ip4addr_ntoa_r(ip_2_ip4(&rl_static_nm), s_nm, (int) sizeof(s_nm)));
    }
}

static void wifi_task(uint64_t now_us) {
    if (RLOG_WIFI_SSID[0] == '\0') {
        return; // 設定なし → 完全惰性
    }

    // ペアリング中は Wi-Fi を一時停止する (BL-45)。
    // CYW43 は Wi-Fi と BLE でラジオを共有するため、STA 接続中は BLE の
    // 広告が受理されても電波に乗らないことがある (ホストから発見できない)。
    // 既存ボンドの接続維持は問題ないので、**発見が必要なペアリング中だけ**
    // Wi-Fi を降りて BLE にラジオを明け渡す。ログはリングに溜まり、復帰後に送る
    {
        static bool wifi_paused_for_pairing = false;
        bool pairing = ble_hid_is_pairing();
        if (pairing && !wifi_paused_for_pairing) {
            wifi_paused_for_pairing = true;
            if (wifi_state != WIFI_IDLE) {
                cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
                udp_close_cleanup();
                wifi_state = WIFI_IDLE;
            }
            remote_log_eventf("WIFI PAUSE (pairing) t=%us", rlog_upt_s());
            oled_set_wifi_state("P");
            return;
        }
        if (!pairing && wifi_paused_for_pairing) {
            wifi_paused_for_pairing = false;
            udp_close_cleanup();
            wifi_state = WIFI_IDLE;
            next_connect_attempt_us = now_us; // すぐ再接続
            remote_log_eventf("WIFI RESUME t=%us", rlog_upt_s());
        }
        if (pairing) {
            return; // ペアリング中は Wi-Fi を触らない (ラジオを BLE に)
        }
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
            remote_log_eventf("WIFI connect start t=%us ssid=%s", rlog_upt_s(), rlog_ssid_masked());
            oled_set_wifi_state(".");
            cyw43_arch_wifi_connect_async(RLOG_WIFI_SSID, RLOG_WIFI_PASSWORD,
                                          RLOG_WIFI_PASSWORD[0] ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN);
            return;

        case WIFI_CONNECTING:
            if (link == CYW43_LINK_UP) {
                wifi_state = WIFI_JOINED;
                udp_open_locked_helper();
                unsigned joined_t = rlog_upt_s();
                const ip4_addr_t *ip = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
                // 接続先 AP の BSSID (AiMesh の「遠いノードに繋いでいる」切り分け用)。
                // cyw43 公開 API は CYW43_THREAD_ENTER を内部で取るため
                // (RSSI の前例と同じ扱い) メインループからそのまま呼べる
                uint8_t bssid[6] = {0};
                char bssid_str[24];
                if (cyw43_wifi_get_bssid(&cyw43_state, bssid) == 0) {
                    snprintf(bssid_str, sizeof(bssid_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
                } else {
                    snprintf(bssid_str, sizeof(bssid_str), "??");
                }
                remote_log_eventf("WIFI JOINED t=%us ip=%s bssid=%s",
                                  joined_t, ip4addr_ntoa(ip), bssid_str);
                oled_set_wifi_state("*");
            } else if (link < 0) {
                reconnect_count++; // BADAUTH (-3) 等の失敗も再接続系として数える
                char code[16]; // OLED 用の short code (幅は診断値の桁数変動に任せる)
                snprintf(code, sizeof(code), "%d", link);
                remote_log_eventf("WIFI FAIL t=%us st=%s rc=%u (retry in %ds)", rlog_upt_s(), code,
                                  (unsigned) reconnect_count, RLOG_CONNECT_RETRY_MS / 1000);
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
                char code[16]; // OLED 用の short code (幅は診断値の桁数変動に任せる)
                remote_log_eventf("WIFI TO t=%us st=%d dhcp=%d js=0x%x nf=0x%x (retry in %ds)",
                                  rlog_upt_s(), link, dhcp_to, js_to, nf_to,
                                  RLOG_CONNECT_RETRY_MS / 1000);
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
                reconnect_count++;
                remote_log_eventf("WIFI LOST t=%us st=%d rc=%u", rlog_upt_s(), link,
                                  (unsigned) reconnect_count);
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

// ---------------------------------------------------------------------------
// BLE ヘルス観測 (BL-43/45 ウェッジ用)
// ---------------------------------------------------------------------------
// 目的: 「BLE は繋がったまま転送だけ止まる」ウェッジを数字で捉える。
// リアルタイム経路 (ble_hid) はカウンタ加算のみで、整形・出力はここに限定する。
//   - BLE STALL / BLE STALL END : 受理済み通知が RLOG_BLE_STALL_MS 途絶えた瞬間と復帰
//   - BLE-HB                    : 15 秒ごとの健全性サマリ (変化が無ければ接続中のみ)
// ok/blk/acl の増え方で「フロー制御待ちで捨てている (blk)」「コントローラが
// バッファ不足を返している (acl)」「そもそも通知が来ていない (ok 停止)」を切り分ける
static uint64_t next_ble_chk_us = 0;
static uint64_t next_ble_hb_us = 0;
static bool ble_stall_active = false;
static uint32_t ble_stall_in_mark = 0;  // 直近チェック時の入力数
static uint32_t ble_stall_ok_mark = 0;  // 直近チェック時の受理数
static uint64_t ble_stall_since_us = 0; // 「入力は来たが受理されていない」開始時刻
static uint32_t ble_stall_in_start = 0; // その開始時点の入力数 (未処理件数の算出用)
static uint64_t ble_last_in_us = 0;     // 最後に input が動いた時刻
static uint32_t ble_hb_in = 0;          // 直近 HB 時点の入力数
static uint32_t ble_hb_loops = 0;       // 直近 HB 時点のループ周回数
static uint64_t ble_hb_us = 0;          // 直近 HB の時刻

static void ble_health_task(uint64_t now_us) {
    if (((int64_t) (next_ble_chk_us - now_us)) > 0) return;
    next_ble_chk_us = now_us + 1000000ULL; // 1 秒周期で判定

    ble_hid_health_t h;
    ble_hid_get_health(&h);

    const bool connected = ble_hid_is_connected();
    const bool ever = (h.last_ok_us != 0);
    const uint32_t tx_ms = ever ? (uint32_t) ((now_us - h.last_ok_us) / 1000ULL) : 0xFFFFFFFFu;

    // 停止判定は「入力は来ているのに、通知が受理されない」状態で行う。
    // 単純な「通知が無い時間」で判定すると、単に無操作のときも停止に見えてしまう。
    // 切断→再接続の直後も入力が来るまで判定しない (前回の受理時刻が古いため)
    const uint32_t in_now = count_ingest;
    const bool in_moved = (in_now != ble_stall_in_mark);
    const bool ok_moved = (h.ok != ble_stall_ok_mark);
    ble_stall_in_mark = in_now;
    ble_stall_ok_mark = h.ok;
    if (in_moved) {
        ble_last_in_us = now_us;
    }
    if (ok_moved) {
        ble_stall_since_us = 0;
    } else if (in_moved && ble_stall_since_us == 0) {
        ble_stall_since_us = now_us;
        ble_stall_in_start = in_now;
    }
    // 入力自体が途絶えたら「未処理の入力」は存在しない → 判定を取り下げる。
    // これが無いと、入力が 1 件来た直後に操作を止めただけで STALL が立ちっぱなしになる
    if (ble_stall_since_us != 0 &&
        (now_us - ble_last_in_us) > (uint64_t) RLOG_BLE_STALL_MS * 1000ULL) {
        ble_stall_since_us = 0;
    }

    const bool stalled = connected && ble_stall_since_us != 0 &&
                         (now_us - ble_stall_since_us) >= (uint64_t) RLOG_BLE_STALL_MS * 1000ULL;
    if (stalled && !ble_stall_active) {
        ble_stall_active = true;
        remote_log_eventf("BLE STALL in=%u/%us ci=%u ok=%u blk=%u acl=%u disc=%u rs=0x%02X",
                          (unsigned) (in_now - ble_stall_in_start),
                          (unsigned) ((now_us - ble_stall_since_us) / 1000000ULL),
                          (unsigned) h.ci, (unsigned) h.ok, (unsigned) h.blocked,
                          (unsigned) h.acl_full, (unsigned) h.disc, (unsigned) h.disc_reason);
    } else if (!stalled && ble_stall_active) {
        ble_stall_active = false;
        remote_log_eventf("BLE STALL END ok=%u tx=%ums", (unsigned) h.ok, (unsigned) tx_ms);
    }

    if (((int64_t) (next_ble_hb_us - now_us)) <= 0) {
        next_ble_hb_us = now_us + RLOG_BLE_HB_MS * 1000ULL;
        // 入力側の観測値: 前回 HB からの入力数 / 入力間隔の最大値 / ループ速度。
        // かくつきは 5 秒平均の in= では見えないので gmax (最大空白) を持つ。
        // 入力が 1 件も無い区間は gmax を 0 にする (無操作を「空白」と呼ばない)
        const uint32_t in_delta = count_ingest - ble_hb_in;
        const uint32_t gmax_ms = (in_delta != 0) ? (ingest_gap_max_us / 1000u) : 0;
        const uint32_t lmax_ms = loop_gap_max_us / 1000u;
        const uint32_t rtt_ms = ack_rtt_last_ms;
        const uint32_t rmax_ms = ack_rtt_max_ms;
        ack_rtt_max_ms = 0;
        uint32_t lps = 0;
        if (ble_hb_us != 0) {
            const uint64_t dt_us = now_us - ble_hb_us;
            if (dt_us != 0) {
                lps = (uint32_t) (((uint64_t) (loop_count - ble_hb_loops)) * 1000000ULL / dt_us);
            }
        }
        ble_hb_in = count_ingest;
        ble_hb_loops = loop_count;
        ble_hb_us = now_us;
        ingest_gap_max_us = 0;
        loop_gap_max_us = 0;

        if (ever) {
            remote_log_eventf("BLE-HB ci=%u ok=%u blk=%u acl=%u disc=%u rs=0x%02X tx=%ums in=%u gmax=%ums lps=%u lmax=%ums rtt=%ums rmax=%ums",
                              (unsigned) h.ci, (unsigned) h.ok, (unsigned) h.blocked,
                              (unsigned) h.acl_full, (unsigned) h.disc,
                              (unsigned) h.disc_reason, (unsigned) tx_ms,
                              (unsigned) in_delta, (unsigned) gmax_ms, (unsigned) lps,
                              (unsigned) lmax_ms, (unsigned) rtt_ms, (unsigned) rmax_ms);
        } else {
            remote_log_eventf("BLE-HB ci=%u ok=0 blk=%u acl=%u disc=%u rs=0x%02X tx=none in=%u gmax=%ums lps=%u lmax=%ums rtt=%ums rmax=%ums",
                              (unsigned) h.ci, (unsigned) h.blocked,
                              (unsigned) h.acl_full, (unsigned) h.disc,
                              (unsigned) h.disc_reason,
                              (unsigned) in_delta, (unsigned) gmax_ms, (unsigned) lps,
                              (unsigned) lmax_ms, (unsigned) rtt_ms, (unsigned) rmax_ms);
        }
    }
}

void remote_log_task(void) {
    uint64_t now_us = time_us_64();
    if (((int64_t) (next_task_us - now_us)) > 0) return;
    next_task_us = now_us + RLOG_FLUSH_INTERVAL_MS * 1000ULL;

    wifi_task(now_us);
    udp_flush_pending();
    ble_health_task(now_us); // BLE ウェッジ観測 (1 秒周期の判定 + 15 秒周期の HB)

    // 5秒毎の生存ピング (リングが詰まっていても出す)
    if (((int64_t) (next_ping_us - now_us)) <= 0) {
        next_ping_us = now_us + RLOG_PING_INTERVAL_MS * 1000ULL;
        size_t pending = (size_t) (rl_tail_abs - rl_head_abs);

        // 診断: DHCP 状態機の位置も添える (lwIP: 2=INIT 6=SELECTING 1=REQUESTING 10=BOUND)。
        // js (cyw43 wifi_join_state) / nf (netif->flags) の 16 進診断は WIFI TO 側に
        // 持たせてあり ALIVE は行長確保のため省略。seq/ack で未達数 (seq-ack) を読む
        struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];
        int dhcp_state = -1;
        int dhcp_tries = 0;
        int32_t rssi = -127; // STA 非稼働・読み取り失敗時のセンチネル
        const char *ip_str = "0.0.0.0";
        static char ip_buf[16];
        cyw43_arch_lwip_begin();
        if (nif != nullptr) {
            struct dhcp *d = netif_dhcp_data(nif);
            if (d != nullptr) {
                dhcp_state = (int) d->state;
                dhcp_tries = (int) d->tries;
            }
            strncpy(ip_buf, ip4addr_ntoa(netif_ip4_addr(nif)), sizeof(ip_buf) - 1);
            ip_buf[sizeof(ip_buf) - 1] = '\0';
            ip_str = ip_buf;
        }
        cyw43_arch_lwip_end();

        // RSSI: 公開 API は内部で CYW43_THREAD_ENTER (async lock, 再入可) を取るので
        // ここから直接呼べる。失敗 (EPERM 等) はセンチネルのまま
        int32_t rssi_val = -127;
        if (cyw43_wifi_get_rssi(&cyw43_state, &rssi_val) == 0) {
            rssi = rssi_val;
        }

        remote_log_eventf(
            "ALIVE up=%ds wifi=%s(%d) ip=%s dhcp=%d/%d rssi=%d m=0x%x ci=%u seq=%u ack=%u rtx=%u rc=%u in=%u fwd=%u q=%u dr=%u dg=%u se=%u",
            (unsigned) (now_us / 1000000ULL), wifi_state_name(wifi_state),
            cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA),
            ip_str, dhcp_state, dhcp_tries, (int) rssi,
            (unsigned) ble_hid_get_connected_slot_mask(), (unsigned) ble_hid_get_conn_interval(),
            (unsigned) rl_seq, (unsigned) rl_last_ack, (unsigned) rtx_count,
            (unsigned) reconnect_count,
            (unsigned) count_ingest, (unsigned) count_forward,
            (unsigned) pending, (unsigned) rl_drops,
            (unsigned) rl_datagrams, (unsigned) rl_send_errors);

        // OLED: Wi-Fi 欄に送信エラー (se) を出す。JOINED なのに 0 送信 — が
        // lwIP で拒否されている (se 増加) のかネットワークに出ていないかを
        // UART 無しで判別できる。se != 0 なら "!<下 2 桁>" (例 "!07")、
        // se == 0 なら従来どおり "*"。非 JOINED の他状態コード (".", "?", "T6" など)
        // は Events 側で設定されるためここでは上書きしない。
        // リング書き込みと同じメインループなので追加ロック不要
        if (wifi_state == WIFI_JOINED) {
            char wi_str[4];
            if (rl_send_errors != 0) {
                snprintf(wi_str, sizeof(wi_str), "!%02u", (unsigned) (rl_send_errors % 100));
            } else {
                snprintf(wi_str, sizeof(wi_str), "*");
            }
            oled_set_wifi_state(wi_str);
        }

        // 宛先 ARP 解決の監視 (状態変化時のみ 1 行、開通後のみ)。
        // lwIP は ARP 未解決でも udp_send が ERR_OK を返して ARP キューに積むため、
        // se も OLED も無反応のまま「宛先が間違っていて ARP request だけ出続ける」
        // 状態が検出できない。5 秒周期で ARP 表を引き、解決/未解決の変化点だけ報告する
        if (udp_state == UDP_OPENED && rl_target_valid && rl_pcb != nullptr) {
            struct eth_addr *ea = nullptr;
            const ip4_addr_t *rip = nullptr;
            struct eth_addr mac = {};
            // ARP 表のエントリはロック外で書き換わり得るためロック内でコピーする
            cyw43_arch_lwip_begin();
            s8_t ai = etharp_find_addr(&cyw43_state.netif[CYW43_ITF_STA],
                                       ip_2_ip4(&rl_target_addr), &ea, &rip);
            int arp_state = (ai >= 0 && ea != nullptr) ? 1 : 2;
            if (arp_state == 1) {
                mac = *ea;
            }
            cyw43_arch_lwip_end();
            if (arp_state != rl_target_arp_state) {
                rl_target_arp_state = arp_state;
                if (arp_state == 1) {
                    remote_log_eventf(
                        "RLOG target resolved mac=%02x:%02x:%02x:%02x:%02x:%02x",
                        mac.addr[0], mac.addr[1], mac.addr[2],
                        mac.addr[3], mac.addr[4], mac.addr[5]);
                } else {
                    remote_log_eventf("RLOG target unresolved ip=%s (arp)", RLOG_TARGET_IP);
                }
            }
        }
    }
}

void remote_log_init(void) {
    int lock_num = spin_lock_claim_unused(true);
    rl_lock = spin_lock_init((uint) lock_num);
    rl_cs_ready = true;
    rl_head_abs = 0;
    rl_tail_abs = 0;
    rl_mirror_abs = 0;
    rl_seq = 0;
    reconnect_count = 0;
    rl_last_ack = 0;
    inflight_seq = 0;
    inflight_end_abs = 0;
    inflight_sent_us = 0;
    first_send_us = 0;
    rtx_count = 0;
    ack_mode_off = false;
    rl_last_err = 0;
    rl_target_arp_state = -1;

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
    // どのビルドが焼かれているかを最初に残す (焼き間違いの検出用)
    remote_log_eventf("BUILD %s %s", REMAPPER_BUILD_TIME, REMAPPER_GIT_SHA);

    remote_log_eventf("RLOG init target=%s:%d ssid=%s ip=%s",
                      RLOG_TARGET_IP, RLOG_TARGET_PORT, rlog_ssid_masked(),
                      rl_static_valid ? "static" : "dhcp");

    const uint64_t boot_grace_us = 3000000ULL; // BLE 起動を優先させる 3 秒猶予
    next_connect_attempt_us = time_us_64() + boot_grace_us;
    next_ping_us = time_us_64() + 5000000ULL;

    remote_log_eventf("RLOG init target=%s:%d ssid=%s ok",
                      RLOG_TARGET_IP, RLOG_TARGET_PORT, rlog_ssid_masked());
}

#endif // ENABLE_REMOTE_LOG
