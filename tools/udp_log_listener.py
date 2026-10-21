#!/usr/bin/env python3
"""BL-39: Wi-Fi UDP リモートログの受信リスナ。

使い方 (収集先マシンで):
    python3 tools/udp_log_listener.py                # 既定 udp/5140・標準出力のみ
    python3 tools/udp_log_listener.py 5140           # ポート指定
    python3 tools/udp_log_listener.py -o log.txt    # 受信行をファイルへ追記 (-a 同義)
    python3 tools/udp_log_listener.py -a -n          # ファイル追記 + 標準出力抑制

オプション:
    -a, --append FILE  受信行を FILE に追記する (開いている間バッファせず都度 flush。
                       夜間観察の取りこぼし対策。既定の標準出力はそのまま)
    -o, --output FILE  --append と同義
    -n, --no-stdout    標準出力へ表示しない (ファイル追記専用モード)
    Ctrl+C で停止。従来どおり tee で保存することも可能:
    python3 tools/udp_log_listener.py | tee bt_udp_$(date +%H%M).log
"""
import argparse
import datetime
import socket
import sys


def main() -> None:
    parser = argparse.ArgumentParser(description="BL-39 UDP log listener")
    parser.add_argument("port", nargs="?", type=int, default=5140)
    parser.add_argument("-a", "--append", "-o", "--output", dest="append",
                        metavar="FILE", help="append received lines to FILE")
    parser.add_argument("-n", "--no-stdout", action="store_true",
                        help="do not print to stdout")
    args = parser.parse_args()

    out = None
    if args.append:
        out = open(args.append, "a", encoding="utf-8")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", args.port))
    target = f"{args.append} + stdout" if (out and not args.no_stdout) else (
        args.append if out else "stdout")
    print(f"listening on udp/{args.port} -> {target}", flush=True)
    try:
        while True:
            data, addr = sock.recvfrom(2048)
            ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            for line in data.decode(errors="replace").splitlines():
                text = f"{ts} {addr[0]:<15} {line}"
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
