#!/usr/bin/env python3
"""UF2 の中身を検算する (焼く前チェック用)。

背景 (2026-10-31): `strings remapper.uf2 | grep CREDIT-STALL` が 0 件になり、
パッチが当たっていないと誤判定した。UF2 は 512 バイトブロックごとに
32 バイトのヘッダを持つため、文字列が 256 バイトのペイロード境界をまたぐと
ヘッダに分断されて grep で見つからなくなる。ペイロードを連結してから探す。

使い方:
    python3 tools/uf2_check.py firmware/artifacts/remapper_anchor_wakeNN.uf2

既定のチェック項目:
    CREDIT-STALL         0x57 監視の再設計 (wake37 以降)
    PARAM-REQ n=%u       接続パラメータ再要求 (wake40 以降)
    clat=%u st=%u        BLE-HB の観測項目 (wake37 以降)
    power-cycle+reload   膠着時の BT コア再投入 (wake36 以降)
    ble_lock=disabled    soak ビルドの目印
    192.168.215.1        誤った送信先 IP (0 件であること)

終了コード: 期待と違えば 1
"""
import struct
import sys


def payload(path):
    data = open(path, 'rb').read()
    out = bytearray()
    for off in range(0, len(data), 512):
        block = data[off:off + 512]
        if len(block) < 512:
            break
        magic0, magic1 = struct.unpack('<2I', block[:8])
        if magic0 != 0x0A324655 or magic1 != 0x9E5D5157:
            break
        size = struct.unpack('<I', block[16:20])[0]
        out += block[32:32 + size]
    return bytes(out)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2

    # (キー, 期待する件数, 説明)
    checks = [
        (b'CREDIT-STALL', 1, '0x57 監視の再設計'),
        (b'PARAM-REQ n=%u', 1, '接続パラメータ再要求'),
        (b'clat=%u st=%u', 1, 'BLE-HB 観測項目'),
        (b'power-cycle+reload', 1, '膠着時の BT コア再投入'),
        (b'ble_lock=disabled', 1, 'soak ビルドの目印'),
        (b'192.168.215.1', 0, '誤った送信先 IP (0 件であること)'),
    ]

    rc = 0
    for path in argv[1:]:
        body = payload(path)
        print('%s (%d bytes)' % (path, len(body)))
        for key, want, note in checks:
            got = body.count(key)
            ok = (got == want) if want else (got == 0)
            print('  %-4s %-22s %d  %s' % ('OK' if ok else 'NG',
                                           key.decode('utf-8', 'replace'), got, note))
            if not ok:
                rc = 1

    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv))
