#!/usr/bin/env python3
"""BL-39: Wi-Fi UDP リモートログの受信リスナ。

使い方 (収集先マシンで):
    python3 tools/udp_log_listener.py                # 既定 udp/5140・標準出力のみ
    python3 tools/udp_log_listener.py 5140           # ポート指定
    python3 tools/udp_log_listener.py -o log.txt     # 受信行をファイルへ追記 (-a 同義)
    python3 tools/udp_log_listener.py -a -n          # ファイル追記 + 標準出力抑制
    python3 tools/udp_log_listener.py --no-ack       # ACK 応答を止める

オプション:
    -a, --append FILE  受信行を FILE に追記する (開いている間バッファせず都度 flush。
                       夜間観察の取りこぼし対策。既定の標準出力はそのまま)
    -o, --output FILE  --append と同義
    -n, --no-stdout    標準出力へ表示しない (ファイル追記専用モード)
    --no-ack           ACK 応答を無効化。既定では受信 datagram 先頭の "#<seq> " を
                       解析し "ACK <seq>" を送信元へ即時返す (受信 datagram 1 個に
                       1 個。5 秒間隔なので負荷は無視できる)。ファーム側では ALIVE
                       の seq= と ack= の差が未達 datagram 数として読める
    Ctrl+C で停止。従来どおり tee で保存することも可能:
    python3 tools/udp_log_listener.py | tee bt_udp_$(date +%H%M).log
"""
import argparse
import datetime
import re
import socket

SEQ_RE = re.compile(r"^#(\d+) ")


def main() -> None:
    parser = argparse.ArgumentParser(description="BL-39 UDP log listener")
    parser.add_argument("port", nargs="?", type=int, default=5140)
    parser.add_argument("-a", "--append", "-o", "--output", dest="append",
                        metavar="FILE", help="append received lines to FILE")
    parser.add_argument("-n", "--no-stdout", action="store_true",
                        help="do not print to stdout")
    parser.add_argument("--no-ack", action="store_true",
                        help="do not reply ACK <seq> to the sender")
    args = parser.parse_args()

    out = None
    if args.append:
        out = open(args.append, "a", encoding="utf-8")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", args.port))
    target = f"{args.append} + stdout" if (out and not args.no_stdout) else (
        args.append if out else "stdout")
    ack_mode = "" if args.no_ack else " + ACK"
    print(f"listening on udp/{args.port} -> {target}{ack_mode}", flush=True)
    try:
        while True:
            data, addr = sock.recvfrom(2048)
            ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            payload = data.decode(errors="replace")
            m = SEQ_RE.match(payload)
            seq = int(m.group(1)) if m else None
            if seq is not None and not args.no_ack:
                # ACK は受信 datagram ごとに 1 個 (送信元のソースポート宛に返す)。
                # ACK 送信失敗は握りつぶして観測を継続する
                try:
                    sock.sendto(f"ACK {seq}".encode(), addr)
                except OSError:
                    pass
            # 表示は seq を付けた形 ("12:34:56.789 192.168.1.250 #1234 ALIVE ...")。
            # seq がない payload (旧ファーム / --no-ack で運用中の欠番) はそのまま表示
            body = payload if seq is None else SEQ_RE.sub("", payload, count=1)
            for line in body.splitlines():
                text = f"{ts} {addr[0]:<15}" + (
                    f" #{seq} {line}" if seq is not None else f" {line}")
                if out is not None:
                    out.write(text + "\n")
                    out.flush()
                if not args.no_stdout:
                    print(text, flush=True)
    finally:
        if out is not None:
            out.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
