#!/usr/bin/env python3
"""BL-39: Wi-Fi UDP リモートログの受信リスナ。

使い方 (収集先マシンで):
    python3 tools/udp_log_listener.py [port]   # 既定 5140
    Ctrl+C で停止。保存したい場合はパイプで:
    python3 tools/udp_log_listener.py | tee bt_udp_$(date +%H%M).log
"""
import datetime
import socket
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5140


def main() -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", PORT))
    print(f"listening on udp/{PORT}", flush=True)
    while True:
        data, addr = sock.recvfrom(2048)
        ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        for line in data.decode(errors="replace").splitlines():
            print(f"{ts} {addr[0]:<15} {line}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
