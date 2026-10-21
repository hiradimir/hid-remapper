// BL-39: Wi-Fi (UDP) 経由のリモートログ送信
//
// 目的: ハウジング運用 (UART 未接続) 時に、ファーム内部状態 (BLE 切断理由・
//       スロット切替・タイマ発火・レポート転送有無など) をリモートで観測する。
//
// 設計原則 (不変条件 7 に準拠):
//   - リアルタイム経路 (BLE コールバック・USB 処理) からはリングバッファへの
//     短時間追記のみ行い、printf・ネットワーク API 等の重い処理は一切しない
//   - Wi-Fi 常駐 / UDP 送信はメインループの remote_log_task() から限定して実施
//   - 有効化はビルドオプション ENABLE_REMOTE_LOG + src/remote_log_net.h
//     (リポジトリには .example のみを置き、実ファイルは gitignore)
//
// 受信側:  nc -lu <port>   (例: nc -lu 5140)
#ifndef REMOTE_LOG_H
#define REMOTE_LOG_H

#include <stdint.h>

#ifndef ENABLE_REMOTE_LOG
#define ENABLE_REMOTE_LOG 0
#endif

#if ENABLE_REMOTE_LOG

void remote_log_init(void);
// メインループから毎ループ呼ぶ (内部でスロットリングされる)
void remote_log_task(void);
// 任意コンテキストから呼べる (リングバッファへの追記のみ)
void remote_log_eventf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// メインループの入力拾いカウンタ (ALIVE ピングに含める)
void remote_log_count_ingest(void);
// メインループの転送実行カウンタ (ALIVE ピングに含める)
void remote_log_count_forward(void);
// メインループ 1 周ごとに呼ぶ (ループ速度・最大停止時間の観測用)
// ENABLE_REMOTE_LOG=0 では空インライン
void remote_log_count_loop(void);

#else
// インラインスタブ (ENABLE_REMOTE_LOG=0 でもビルドが通る形を維持)
static inline void remote_log_init(void) {}
static inline void remote_log_task(void) {}
static inline void remote_log_eventf(const char *fmt, ...) { (void)fmt; }
static inline void remote_log_count_ingest(void) {}
static inline void remote_log_count_forward(void) {}
static inline void remote_log_count_loop(void) {}
#endif

#endif // REMOTE_LOG_H
